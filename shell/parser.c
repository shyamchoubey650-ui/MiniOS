/* Turns tokens into command lines: argv plus optional output redirection, and pipelines of them. */
#include "include/shell.h"

int parse_pipeline(const char *line, char *buf, size_t bufsz, pipeline_t *out) {
    token_t tok[SHELL_MAX_TOKENS];
    int n = tokenize(line, buf, bufsz, tok, SHELL_MAX_TOKENS);

    if (!out) return PARSE_ERR_BUFFER;
    out->nstages = 0;
    for (int s = 0; s < PIPE_MAX_STAGES; s++) { out->stage[s].argc = 0; out->stage[s].argv[0] = NULL; out->stage[s].redir = REDIR_NONE; out->stage[s].redir_path = NULL; }

    if (n == TOK_ERR_UNTERMINATED) return PARSE_ERR_QUOTE;
    if (n == TOK_ERR_TOO_MANY)     return PARSE_ERR_TOO_MANY;
    if (n < 0)                     return PARSE_ERR_BUFFER;
    if (n == 0)                    return PARSE_EMPTY;

    int s = 0;                                    /* the stage being filled */
    for (int i = 0; i < n; i++) {
        command_line_t *c = &out->stage[s];
        if (tok[i].type == TOK_WORD) {
            if (c->argc >= SHELL_MAX_ARGS) return PARSE_ERR_TOO_MANY;
            c->argv[c->argc++] = tok[i].text;
            c->argv[c->argc] = NULL;
            continue;
        }
        if (tok[i].type == TOK_PIPE) {
            if (c->argc == 0) return c->redir != REDIR_NONE ? PARSE_ERR_NO_COMMAND : PARSE_ERR_EMPTY_STAGE;
            if (c->redir != REDIR_NONE) return PARSE_ERR_PIPE_REDIR;
            if (++s >= PIPE_MAX_STAGES) return PARSE_ERR_TOO_MANY_STAGES;
            continue;
        }
        /* redirection operator */
        if (c->redir != REDIR_NONE)                   return PARSE_ERR_REDIR_TWICE;
        if (i + 1 >= n || tok[i + 1].type != TOK_WORD) return PARSE_ERR_REDIR_TARGET;
        c->redir = (tok[i].type == TOK_REDIR_APPEND) ? REDIR_APPEND : REDIR_TRUNCATE;
        c->redir_path = tok[i + 1].text;
        i++;                                          /* consume the file name */
    }
    if (out->stage[s].argc == 0) return s > 0 ? PARSE_ERR_EMPTY_STAGE : PARSE_ERR_NO_COMMAND;
    out->nstages = s + 1;
    return PARSE_OK;
}

/* ---------- command lists ---------- */
static bool blank(const char *s, size_t len) { for (size_t i = 0; i < len; i++) if (s[i] != ' ' && s[i] != '\t') return false; return true; }

int split_command_list(const char *line, segment_t *seg, int max) {
    int n = 0; size_t start = 0, i = 0; connector_t next = CONN_FIRST; char quote = 0;
    if (!line || !seg || max < 1) return PARSE_ERR_BUFFER;
    for (;; i++) {
        char c = line[i];
        bool end = c == '\0';
        connector_t op = CONN_FIRST; size_t oplen = 0;
        if (!end) {
            if (quote) { if (c == quote) quote = 0; continue; }
            if (c == '"' || c == '\'') { quote = c; continue; }
            if (c == ';') { op = CONN_SEMI; oplen = 1; }
            else if (c == '&' && line[i + 1] == '&') { op = CONN_AND; oplen = 2; }
            else if (c == '&') return PARSE_ERR_BACKGROUND;
            else if (c == '|' && line[i + 1] == '|') { op = CONN_OR; oplen = 2; }
            else continue;
        } else if (quote) return PARSE_ERR_QUOTE;
        size_t len = i - start;
        if (blank(line + start, len)) {
            if (end && n > 0 && next == CONN_SEMI) break;                      /* "a ;" : a trailing semicolon is fine */
            if (end && n == 0) return 0;                                       /* an empty line */
            return PARSE_ERR_LIST_EMPTY;
        }
        if (n >= max) return PARSE_ERR_LIST_TOO_LONG;
        seg[n].text = line + start; seg[n].len = len; seg[n].conn = next; n++;
        if (end) break;
        next = op; i += oplen - 1; start = i + 1;
    }
    return n;
}

/* ---------- keywords ---------- */
static const struct { const char *word; keyword_t kw; bool takes_command; } kwtab[] = {
    { "if", KW_IF, true }, { "then", KW_THEN, true }, { "elif", KW_ELIF, true }, { "else", KW_ELSE, true }, { "fi", KW_FI, false },
    { "while", KW_WHILE, true }, { "for", KW_FOR, false }, { "do", KW_DO, true }, { "done", KW_DONE, false },
    { "break", KW_BREAK, false }, { "continue", KW_CONTINUE, false },
};

/* The keyword at the start of text[0..len), and where the rest begins. */
static keyword_t leading_keyword(const char *t, size_t len, size_t *rest, bool *takes) {
    size_t i = 0, s;
    while (i < len && (t[i] == ' ' || t[i] == '\t')) i++;
    s = i;
    while (i < len && t[i] != ' ' && t[i] != '\t') i++;
    size_t wl = i - s;
    for (size_t k = 0; k < sizeof kwtab / sizeof kwtab[0]; k++) {
        size_t n = 0; while (kwtab[k].word[n]) n++;
        if (n != wl) continue;
        size_t j = 0; while (j < n && t[s + j] == kwtab[k].word[j]) j++;
        if (j != n) continue;
        while (i < len && (t[i] == ' ' || t[i] == '\t')) i++;
        *rest = i; *takes = kwtab[k].takes_command;
        return kwtab[k].kw;
    }
    return KW_NONE;
}

static int push_x(xseg_t *out, int n, int max, const char *t, size_t len, connector_t conn, keyword_t kw) {
    if (n >= max) return PARSE_ERR_LIST_TOO_LONG;
    out[n].text = t; out[n].len = len; out[n].conn = conn; out[n].kw = kw;
    return n + 1;
}

/* One segment may hold several keywords ("then if x"): every keyword becomes its own entry, the remainder follows. */
static int emit_x(xseg_t *out, int n, int max, const char *t, size_t len, connector_t conn) {
    size_t rest; bool takes;
    keyword_t kw = leading_keyword(t, len, &rest, &takes);
    if (kw == KW_NONE) return push_x(out, n, max, t, len, conn, KW_NONE);
    if (kw == KW_FOR) {                                                   /* the header stays in one piece */
        if (rest >= len) return push_x(out, n, max, t + len, 0, conn, KW_FOR);
        return push_x(out, n, max, t + rest, len - rest, conn, KW_FOR);
    }
    n = push_x(out, n, max, t + len, 0, conn, kw);
    if (n < 0) return n;
    if (rest >= len) return n;                                            /* nothing after the keyword */
    if (!takes) return PARSE_ERR_KW_TEXT;                                 /* "fi echo", "done | sort", "break now" */
    return emit_x(out, n, max, t + rest, len - rest, CONN_FIRST);
}

int expand_keywords(const segment_t *raw, int n, xseg_t *out, int max) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        m = emit_x(out, m, max, raw[i].text, raw[i].len, raw[i].conn);
        if (m < 0) return m;
    }
    return m;
}

bool command_list_incomplete(const char *line) {
    segment_t seg[LIST_MAX_SEGMENTS]; xseg_t x[XSEG_MAX];
    int n = split_command_list(line, seg, LIST_MAX_SEGMENTS);
    if (n <= 0) return false;
    int m = expand_keywords(seg, n, x, XSEG_MAX);
    if (m < 0) return false;
    int depth = 0;
    for (int i = 0; i < m; i++) {
        if (x[i].kw == KW_IF || x[i].kw == KW_WHILE || x[i].kw == KW_FOR) depth++;
        else if (x[i].kw == KW_FI || x[i].kw == KW_DONE) depth--;
    }
    return depth > 0;
}

int parse_command_line(const char *line, char *buf, size_t bufsz, command_line_t *out) {
    pipeline_t p;
    if (!out) return PARSE_ERR_BUFFER;
    out->argc = 0; out->argv[0] = NULL; out->redir = REDIR_NONE; out->redir_path = NULL;
    int rc = parse_pipeline(line, buf, bufsz, &p);
    if (rc != PARSE_OK) return rc;
    if (p.nstages != 1) return PARSE_ERR_PIPE;
    *out = p.stage[0];
    return PARSE_OK;
}

const char *parse_error_string(int code) {
    switch (code) {
        case PARSE_ERR_QUOTE:        return "unterminated quote";
        case PARSE_ERR_TOO_MANY:     return "too many arguments";
        case PARSE_ERR_BUFFER:       return "input too long";
        case PARSE_ERR_REDIR_TARGET: return "missing file name after '>'";
        case PARSE_ERR_REDIR_TWICE:  return "only one output redirection allowed";
        case PARSE_ERR_NO_COMMAND:   return "redirection without a command";
        case PARSE_ERR_EMPTY_STAGE:  return "missing command next to '|'";
        case PARSE_ERR_PIPE_REDIR:   return "only the last command of a pipeline can use '>' or '>>'";
        case PARSE_ERR_TOO_MANY_STAGES: return "a pipeline can have at most 4 commands";
        case PARSE_ERR_PIPE:         return "a pipeline is not allowed here";
        case PARSE_ERR_LIST_EMPTY:   return "missing command next to ';', '&&' or '||'";
        case PARSE_ERR_BACKGROUND:   return "background jobs ('&') are not supported: use 'spawn'";
        case PARSE_ERR_LIST_TOO_LONG: return "a command list can have at most 16 commands";
        case PARSE_ERR_KW_TEXT:      return "text after 'fi', 'done', 'break' or 'continue' is not allowed";
        default:                     return "unknown parse error";
    }
}
