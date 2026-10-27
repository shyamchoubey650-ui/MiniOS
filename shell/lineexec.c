#include "include/lineexec.h"
#include "include/shellvars.h"
#include "include/screen.h"
#include "include/string.h"
#include "include/memory.h"

typedef struct {
    const xseg_t *x; int n;
    pipe_lookup_fn lookup; bool (*is_owner)(void);
    int budget;            /* commands this line may still run */
    int loop_depth;        /* > 0 while a loop body runs: break and continue are legal */
    int ctl;               /* 0 = none, 1 = break, 2 = continue: set by the keyword, consumed by the loop */
    const char *err;       /* first syntax error (found in the dry run) */
    char errword[12];
    bool stop;             /* budget used up or Ctrl+C: unwind everything */
    int stop_status;
} ctx_t;

static bool (*interrupt_check)(void);
void line_set_interrupt_check(bool (*pressed)(void)) { interrupt_check = pressed; }

static const char *kw_name(keyword_t k) {
    switch (k) {
        case KW_IF: return "if"; case KW_THEN: return "then"; case KW_ELIF: return "elif"; case KW_ELSE: return "else"; case KW_FI: return "fi";
        case KW_WHILE: return "while"; case KW_FOR: return "for"; case KW_DO: return "do"; case KW_DONE: return "done";
        case KW_BREAK: return "break"; case KW_CONTINUE: return "continue"; default: return "?";
    }
}
static void fail(ctx_t *c, const char *msg, keyword_t about) {
    if (c->err) return;                                   /* keep the first problem */
    c->err = msg;
    const char *w = about == KW_NONE ? "" : kw_name(about);
    size_t i = 0; while (w[i] && i + 1 < sizeof c->errword) { c->errword[i] = w[i]; i++; } c->errword[i] = '\0';
}
static bool is_opener(keyword_t k) { return k == KW_IF || k == KW_WHILE || k == KW_FOR; }
static bool is_closer(keyword_t k) { return k == KW_FI || k == KW_DONE; }
static bool is_marker(keyword_t k) { return k == KW_THEN || k == KW_ELIF || k == KW_ELSE || k == KW_DO; }

/* ---------- running one simple command ---------- */
/* The buffers (about 1.4 KB) live on the heap: the shell has a 16 KiB stack and `source` can nest four levels deep. */
typedef struct { char raw[SHELL_LINE_MAX * 2], aliased[SHELL_LINE_MAX * 2], line[SHELL_LINE_MAX * 2], buf[SHELL_LINE_MAX * 2]; pipeline_t pl; } seg_work_t;

static int run_segment(const char *text, size_t len, pipe_lookup_fn lookup, bool (*is_owner)(void)) {
    seg_work_t *w = kmalloc(sizeof *w);
    bool was; int status;
    if (!w) { screen_puts("Error: out of memory.\n"); return 1; }
    if (len >= sizeof w->raw) len = sizeof w->raw - 1;
    for (size_t i = 0; i < len; i++) w->raw[i] = text[i];
    w->raw[len] = '\0';
    /* aliases and $variables are expanded just before parsing, per command: the history keeps what was typed */
    if (sv_expand_alias(w->raw, w->aliased, sizeof w->aliased, &was) != SV_OK || sv_expand_vars(w->aliased, w->line, sizeof w->line) != SV_OK) {
        screen_puts("Error: the line is too long after expanding aliases and variables.\n");
        kfree(w);
        return 2;
    }
    int rc = parse_pipeline(w->line, w->buf, sizeof w->buf, &w->pl);
    if (rc == PARSE_EMPTY) { kfree(w); return 0; }
    if (rc != PARSE_OK) { screen_puts("Error: "); screen_puts(parse_error_string(rc)); screen_putc('\n'); kfree(w); return 2; }
    status = pipeline_run(&w->pl, lookup, is_owner);
    kfree(w);
    return status;
}

static int exec_seq(ctx_t *c, int lo, int hi, bool dry);

/* Index of the fi/done that closes the if/while/for at x[i]; -1 (with an error recorded) if there is none. */
static int find_close(ctx_t *c, int i) {
    keyword_t stack[8]; int sp = 0;
    for (int j = i; j < c->n; j++) {
        keyword_t k = c->x[j].kw;
        if (is_opener(k)) { if (sp >= 8) { fail(c, "syntax error: blocks are nested too deeply (limit 8)", KW_NONE); return -1; } stack[sp++] = k; }
        else if (is_closer(k)) {
            if (sp == 0) { fail(c, "syntax error: unexpected", k); return -1; }
            keyword_t open = stack[--sp];
            if ((open == KW_IF) != (k == KW_FI)) { fail(c, open == KW_IF ? "syntax error: 'fi' is needed to close an 'if', found" : "syntax error: 'done' is needed to close a loop, found", k); return -1; }
            if (sp == 0) return j;
        }
    }
    fail(c, stack[0] == KW_IF ? "syntax error: missing 'fi'" : "syntax error: missing 'done'", KW_NONE);
    return -1;
}

/* Top-level markers (then/elif/else/do) strictly between open and close. Returns their count, -1 on overflow. */
static int collect_markers(ctx_t *c, int open, int close, int *idx, int max) {
    int depth = 0, m = 0;
    for (int j = open + 1; j < close; j++) {
        keyword_t k = c->x[j].kw;
        if (is_opener(k)) depth++;
        else if (is_closer(k)) depth--;
        else if (depth == 0 && is_marker(k)) { if (m >= max) return -1; idx[m++] = j; }
    }
    return m;
}

static bool range_empty(ctx_t *c, int lo, int hi) { (void)c; return lo >= hi; }

static int exec_if(ctx_t *c, int open, int close, bool dry) {
    int mk[16]; int m = collect_markers(c, open, close, mk, 16);
    if (m < 0) { fail(c, "syntax error: too many elif branches", KW_NONE); return 2; }
    /* order: then ( elif then )* [ else ] */
    int pos = 0; bool has_else = false;
    int cond_lo[8], cond_hi[8], body_lo[8], body_hi[8], nb = 0, else_lo = 0, else_hi = 0;
    int cl = open + 1;                                           /* the condition starts right after 'if' */
    for (;;) {
        if (pos >= m || c->x[mk[pos]].kw != KW_THEN) { fail(c, "syntax error: 'then' expected after the condition", KW_NONE); return 2; }
        if (nb >= 8) { fail(c, "syntax error: too many elif branches", KW_NONE); return 2; }
        cond_lo[nb] = cl; cond_hi[nb] = mk[pos];
        body_lo[nb] = mk[pos] + 1;
        pos++;
        int next = pos < m ? mk[pos] : close;
        body_hi[nb] = next; nb++;
        if (pos >= m) break;
        if (c->x[mk[pos]].kw == KW_ELIF) { cl = mk[pos] + 1; pos++; continue; }
        if (c->x[mk[pos]].kw == KW_ELSE) {
            has_else = true; else_lo = mk[pos] + 1; else_hi = close; pos++;
            if (pos < m) { fail(c, "syntax error: nothing may follow 'else' except 'fi', found", c->x[mk[pos]].kw); return 2; }
            break;
        }
        fail(c, "syntax error: unexpected", c->x[mk[pos]].kw); return 2;
    }
    for (int b = 0; b < nb; b++) {
        if (range_empty(c, cond_lo[b], cond_hi[b])) { fail(c, "syntax error: empty condition", KW_NONE); return 2; }
        if (range_empty(c, body_lo[b], body_hi[b])) { fail(c, "syntax error: empty block after 'then'", KW_NONE); return 2; }
    }
    if (has_else && range_empty(c, else_lo, else_hi)) { fail(c, "syntax error: empty block after 'else'", KW_NONE); return 2; }
    if (dry) {                                                   /* check every branch, run none */
        for (int b = 0; b < nb; b++) { exec_seq(c, cond_lo[b], cond_hi[b], true); exec_seq(c, body_lo[b], body_hi[b], true); }
        if (has_else) exec_seq(c, else_lo, else_hi, true);
        return 0;
    }
    for (int b = 0; b < nb; b++) {
        int st = exec_seq(c, cond_lo[b], cond_hi[b], false);
        if (c->stop || c->ctl) return st;
        if (st == 0) return exec_seq(c, body_lo[b], body_hi[b], false);
    }
    if (has_else) return exec_seq(c, else_lo, else_hi, false);
    return 0;                                                    /* no branch taken */
}

static bool interrupted(ctx_t *c) {
    if (c->stop) return true;
    if (interrupt_check && interrupt_check()) { screen_puts("^C\n"); c->stop = true; c->stop_status = 130; return true; }
    return false;
}

static int exec_while(ctx_t *c, int open, int close, bool dry) {
    int mk[4]; int m = collect_markers(c, open, close, mk, 4);
    if (m != 1 || c->x[mk[0]].kw != KW_DO) {
        if (m >= 1 && c->x[mk[0]].kw != KW_DO) fail(c, "syntax error: unexpected", c->x[mk[0]].kw);
        else fail(c, "syntax error: 'do' expected after the condition of 'while'", KW_NONE);
        return 2;
    }
    int cond_lo = open + 1, cond_hi = mk[0], body_lo = mk[0] + 1, body_hi = close;
    if (range_empty(c, cond_lo, cond_hi)) { fail(c, "syntax error: empty condition", KW_NONE); return 2; }
    if (range_empty(c, body_lo, body_hi)) { fail(c, "syntax error: empty block after 'do'", KW_NONE); return 2; }
    if (dry) { exec_seq(c, cond_lo, cond_hi, true); c->loop_depth++; exec_seq(c, body_lo, body_hi, true); c->loop_depth--; return 0; }
    int status = 0;
    for (int turn = 0; ; turn++) {
        if (turn >= LINE_MAX_LOOP_TURNS) { screen_puts("Error: the loop was stopped after 1000 turns\n"); c->stop = true; c->stop_status = 1; return 1; }
        if (interrupted(c)) return c->stop_status;
        int cs = exec_seq(c, cond_lo, cond_hi, false);
        if (c->stop) return c->stop_status;
        if (cs != 0) break;
        c->loop_depth++;
        status = exec_seq(c, body_lo, body_hi, false);
        c->loop_depth--;
        if (c->stop) return c->stop_status;
        if (c->ctl == 1) { c->ctl = 0; break; }
        if (c->ctl == 2) c->ctl = 0;
    }
    return status;
}

/* "VAR in w1 w2 ..." -> variable name and words (variables expanded unless this is the dry run). Returns the word count or -1. */
#define FOR_MAX_WORDS 28
static int parse_for(ctx_t *c, const xseg_t *h, bool dry, char *var, size_t varcap, char words[][VAR_VALUE_MAX + 1]) {
    char raw[SHELL_LINE_MAX * 2], exp[SHELL_LINE_MAX * 2], tb[SHELL_LINE_MAX * 2]; token_t tok[SHELL_MAX_TOKENS];
    size_t len = h->len < sizeof raw - 1 ? h->len : sizeof raw - 1;
    for (size_t i = 0; i < len; i++) raw[i] = h->text[i];
    raw[len] = '\0';
    const char *src = raw;
    if (!dry) { if (sv_expand_vars(raw, exp, sizeof exp) != SV_OK) { fail(c, "for: the word list is too long after expanding variables", KW_NONE); return -1; } src = exp; }
    int n = tokenize(src, tb, sizeof tb, tok, SHELL_MAX_TOKENS);
    if (n < 0) { fail(c, "for: bad word list (unterminated quote or too long)", KW_NONE); return -1; }
    if (n < 1) { fail(c, "for: a variable name is expected: for VAR in words ; do ... ; done", KW_NONE); return -1; }
    for (int i = 0; i < n; i++) if (tok[i].type != TOK_WORD) { fail(c, "for: '>' and '|' cannot be used in the word list", KW_NONE); return -1; }
    if (!sv_name_valid(tok[0].text)) { fail(c, "for: the variable name must be letters, digits and _ (not starting with a digit)", KW_NONE); return -1; }
    if (n < 2 || strcmp(tok[1].text, "in")) { fail(c, "for: 'in' expected after the variable name", KW_NONE); return -1; }
    size_t vl = strlen(tok[0].text); if (vl >= varcap) vl = varcap - 1;
    for (size_t i = 0; i < vl; i++) var[i] = tok[0].text[i];
    var[vl] = '\0';
    int count = 0;
    for (int i = 2; i < n; i++) {
        if (count >= FOR_MAX_WORDS) { fail(c, "for: at most 28 words", KW_NONE); return -1; }
        size_t l = strlen(tok[i].text); if (l > VAR_VALUE_MAX) { fail(c, "for: a word is longer than 63 characters", KW_NONE); return -1; }
        for (size_t k = 0; k <= l; k++) words[count][k] = tok[i].text[k];
        count++;
    }
    return count;
}

static int exec_for(ctx_t *c, int open, int close, bool dry) {
    char var[VAR_NAME_MAX + 2];
    /* the words live on the heap: nested loops must not eat the (small) kernel stack */
    char (*words)[VAR_VALUE_MAX + 1] = kmalloc(sizeof(char[FOR_MAX_WORDS][VAR_VALUE_MAX + 1]));
    if (!words) { fail(c, "for: out of memory", KW_NONE); return 2; }
    int nw = parse_for(c, &c->x[open], dry, var, sizeof var, words);
    int status = 2;
    if (nw < 0) goto done;
    if (open + 1 >= close || c->x[open + 1].kw != KW_DO) { fail(c, "syntax error: 'do' expected after the word list of 'for'", KW_NONE); goto done; }
    int mk[4]; int m = collect_markers(c, open, close, mk, 4);
    if (m != 1) { fail(c, "syntax error: unexpected", m > 1 ? c->x[mk[1]].kw : KW_NONE); goto done; }
    int body_lo = open + 2, body_hi = close;
    if (range_empty(c, body_lo, body_hi)) { fail(c, "syntax error: empty block after 'do'", KW_NONE); goto done; }
    if (dry) { c->loop_depth++; exec_seq(c, body_lo, body_hi, true); c->loop_depth--; status = 0; goto done; }
    status = 0;
    for (int w = 0; w < nw; w++) {
        if (interrupted(c)) { status = c->stop_status; break; }
        int rc = var_set(var, words[w], false);
        if (rc != SV_OK) { screen_puts("Error: for: cannot assign to the variable '"); screen_puts(var); screen_puts("'\n"); status = 1; break; }
        c->loop_depth++;
        status = exec_seq(c, body_lo, body_hi, false);
        c->loop_depth--;
        if (c->stop) { status = c->stop_status; break; }
        if (c->ctl == 1) { c->ctl = 0; break; }
        if (c->ctl == 2) c->ctl = 0;
    }
done:
    kfree(words);
    return status;
}

static int exec_seq(ctx_t *c, int lo, int hi, bool dry) {
    int status = 0;
    for (int i = lo; i < hi; ) {
        const xseg_t *s = &c->x[i];
        connector_t conn = i == lo ? CONN_FIRST : s->conn;
        bool skip = (conn == CONN_AND && status != 0) || (conn == CONN_OR && status == 0);
        if (s->kw == KW_NONE) {
            if (!dry && !skip) {
                if (--c->budget < 0) { screen_puts("Error: this line ran more than 5000 commands and was stopped\n"); c->stop = true; c->stop_status = 1; return 1; }
                status = run_segment(s->text, s->len, c->lookup, c->is_owner);
                sv_set_status(status);
            }
            i++;
        } else if (is_opener(s->kw)) {
            int close = find_close(c, i);
            if (close < 0) return 2;
            int r = 0;
            if (dry || !skip)                                    /* a skipped block is not even looked at (the dry run already checked it) */
                r = s->kw == KW_IF ? exec_if(c, i, close, dry) : s->kw == KW_WHILE ? exec_while(c, i, close, dry) : exec_for(c, i, close, dry);
            if (c->err) return 2;
            if (!dry && !skip) { status = r; sv_set_status(status); }
            i = close + 1;
        } else if (s->kw == KW_BREAK || s->kw == KW_CONTINUE) {
            if (c->loop_depth == 0) { fail(c, "syntax error: only allowed inside a loop:", s->kw); return 2; }
            if (!dry && !skip) { c->ctl = s->kw == KW_BREAK ? 1 : 2; sv_set_status(0); return 0; }      /* break and continue themselves succeed */
            i++;
        } else {
            fail(c, "syntax error: unexpected", s->kw);              /* then/elif/else/do outside their block, or a stray fi/done */
            return 2;
        }
        if (c->stop) return c->stop_status;
        if (c->ctl) return status;
    }
    return status;
}

/* The per-line state (segments, keyword list, context) is about 2 KB. It lives on the heap because the shell runs on a 16 KiB stack and
 * `source` can nest four levels deep, each with its own line. */
typedef struct { segment_t seg[LIST_MAX_SEGMENTS]; xseg_t x[XSEG_MAX]; ctx_t c; } work_t;

int line_execute(const char *raw, pipe_lookup_fn lookup, bool (*is_owner)(void), bool *ran) {
    *ran = false;
    work_t *w = kmalloc(sizeof *w);
    if (!w) { *ran = true; screen_puts("Error: out of memory.\n"); sv_set_status(1); return 1; }
    int status = 0;
    int n = split_command_list(raw, w->seg, LIST_MAX_SEGMENTS);
    if (n == 0) { kfree(w); return 0; }
    if (n > 0) n = expand_keywords(w->seg, n, w->x, XSEG_MAX);
    if (n < 0) {
        *ran = true;
        screen_puts("Error: "); screen_puts(parse_error_string(n)); screen_putc('\n');
        sv_set_status(2);
        kfree(w);
        return 2;
    }
    ctx_t *c = &w->c;
    memset(c, 0, sizeof *c);
    c->x = w->x; c->n = n; c->lookup = lookup; c->is_owner = is_owner; c->budget = LINE_MAX_COMMANDS;
    exec_seq(c, 0, n, true);                                     /* dry run: every syntax error is reported before anything runs */
    if (c->err) {
        *ran = true;
        screen_puts("Error: "); screen_puts(c->err);
        if (c->errword[0]) { screen_puts(" '"); screen_puts(c->errword); screen_putc('\''); }
        screen_putc('\n');
        sv_set_status(2);
        kfree(w);
        return 2;
    }
    const xseg_t *x = c->x;
    memset(c, 0, sizeof *c);
    c->x = x; c->n = n; c->lookup = lookup; c->is_owner = is_owner; c->budget = LINE_MAX_COMMANDS;
    status = exec_seq(c, 0, n, false);
    if (c->stop) status = c->stop_status;
    *ran = true;
    sv_set_status(status);
    kfree(w);
    return status;
}
