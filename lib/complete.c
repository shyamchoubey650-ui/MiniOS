#include "include/complete.h"

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static bool   starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return false; return true; }
static bool   eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

bool comp_name_safe(const char *n) {
    if (!n || !*n) return false;
    for (; *n; n++) {
        char c = *n;
        if ((unsigned char)c <= ' ' || (unsigned char)c > 126) return false;
        if (c == '"' || c == '\'' || c == ';' || c == '|' || c == '&' || c == '<' || c == '>' || c == '$' || c == '\\' || c == '(' || c == ')' || c == '*' || c == '?' || c == '`') return false;
    }
    return true;
}

typedef struct {
    const char *prefix; size_t plen;          /* only names starting with this */
    const char *dir; size_t dlen;             /* text put in front of each name in the final word */
    bool allow_hidden;
    comp_result_t *r;
    char full[COMP_MAX][COMP_TEXT_MAX];       /* the whole replacement word of each candidate */
    bool is_var, braces;
} gather_t;

static void gather(const char *name, bool is_dir, void *vctx) {
    gather_t *g = vctx;
    if (!comp_name_safe(name) || !starts(name, g->prefix)) return;
    if (!g->allow_hidden && name[0] == '.') return;
    size_t nl = slen(name);
    comp_result_t *r = g->r;
    /* duplicates (a command that is also a keyword) are listed once */
    for (size_t i = 0; i < r->ncand && i < COMP_MAX; i++) if (eq(r->cand[i], name)) return;
    r->total++;
    if (r->ncand >= COMP_MAX) return;
    if (nl >= COMP_NAME_MAX) return;
    size_t i = r->ncand;
    /* keep the list sorted: insert at the right place */
    size_t at = i;
    while (at > 0) { const char *p = r->cand[at - 1]; size_t k = 0; while (p[k] && p[k] == name[k]) k++; if ((unsigned char)p[k] > (unsigned char)name[k]) at--; else break; }
    for (size_t m = i; m > at; m--) { for (size_t k = 0; k < COMP_NAME_MAX; k++) r->cand[m][k] = r->cand[m - 1][k]; r->cand_dir[m] = r->cand_dir[m - 1]; for (size_t k = 0; k < COMP_TEXT_MAX; k++) g->full[m][k] = g->full[m - 1][k]; }
    for (size_t k = 0; k <= nl; k++) r->cand[at][k] = name[k];
    r->cand_dir[at] = is_dir;
    size_t w = 0;
    if (g->is_var) { g->full[at][w++] = '$'; if (g->braces) g->full[at][w++] = '{'; }
    else for (size_t k = 0; k < g->dlen && w + 1 < COMP_TEXT_MAX; k++) g->full[at][w++] = g->dir[k];
    for (size_t k = 0; k < nl && w + 1 < COMP_TEXT_MAX; k++) g->full[at][w++] = name[k];
    if (is_dir && !g->is_var && w + 1 < COMP_TEXT_MAX) g->full[at][w++] = '/';
    if (g->braces && w + 1 < COMP_TEXT_MAX) g->full[at][w++] = '}';
    g->full[at][w] = '\0';
    r->ncand++;
}

static const char *const keywords[] = { "if", "then", "elif", "else", "fi", "while", "for", "do", "done", "break", "continue" };
static const char *const cmd_leaders[] = { "sudo", "then", "do", "else", "if", "elif", "while" };

comp_kind_t complete_word(const char *line, size_t cursor, const comp_env_t *env, comp_result_t *r) {
    static gather_t g;                                           /* about 8 KB: not on the shell's small stack (one shell, never re-entered) */
    r->kind = COMP_NONE; r->ncand = r->total = 0; r->text[0] = '\0'; r->append_space = false; r->word_start = cursor;
    size_t ws = cursor;
    while (ws > 0 && line[ws - 1] != ' ' && line[ws - 1] != '\t' && line[ws - 1] != ';' && line[ws - 1] != '|' && line[ws - 1] != '&' && line[ws - 1] != '>' && line[ws - 1] != '<') ws--;
    r->word_start = ws;
    static char word[COMP_TEXT_MAX]; size_t wl = cursor - ws;                 /* static: g keeps a pointer into it, and the shell stack is small */
    if (wl >= sizeof word) return COMP_NONE;
    for (size_t i = 0; i < wl; i++) { word[i] = line[ws + i]; if (word[i] == '"' || word[i] == '\'' || word[i] == '\\') return COMP_NONE; }
    word[wl] = '\0';

    g.r = r; g.is_var = g.braces = false; g.allow_hidden = false; g.dlen = 0; g.dir = ""; g.prefix = word; g.plen = wl;

    if (word[0] == '$') {                                         /* variables */
        g.is_var = true;
        g.braces = word[1] == '{';
        g.prefix = word + (g.braces ? 2 : 1);
        if (!env->variables) return COMP_NONE;
        env->variables(gather, &g);
    } else {
        /* command position? look at what stands before the word */
        size_t b = ws; while (b > 0 && (line[b - 1] == ' ' || line[b - 1] == '\t')) b--;
        bool command_pos = false;
        if (b == 0) command_pos = true;
        else if (line[b - 1] == ';' || line[b - 1] == '|' || line[b - 1] == '&') command_pos = true;
        else if (line[b - 1] != '>' && line[b - 1] != '<') {
            size_t pe = b, pb = b; while (pb > 0 && line[pb - 1] != ' ' && line[pb - 1] != '\t' && line[pb - 1] != ';' && line[pb - 1] != '|' && line[pb - 1] != '&') pb--;
            for (size_t k = 0; k < sizeof cmd_leaders / sizeof cmd_leaders[0]; k++) {
                size_t ll = slen(cmd_leaders[k]);
                if (pe - pb == ll) { size_t i = 0; while (i < ll && line[pb + i] == cmd_leaders[k][i]) i++; if (i == ll) command_pos = true; }
            }
        }
        bool has_slash = false; for (size_t i = 0; i < wl; i++) if (word[i] == '/') has_slash = true;
        if (command_pos && !has_slash) {
            if (!env->commands) return COMP_NONE;
            env->commands(gather, &g);
            for (size_t k = 0; k < sizeof keywords / sizeof keywords[0]; k++) gather(keywords[k], false, &g);
        } else {
            size_t slash = wl; while (slash > 0 && word[slash - 1] != '/') slash--;      /* directory part = word[0..slash) */
            static char dir[COMP_TEXT_MAX];
            for (size_t i = 0; i < slash; i++) dir[i] = word[i];
            dir[slash] = '\0';
            g.dir = dir; g.dlen = slash; g.prefix = word + slash; g.plen = wl - slash;
            g.allow_hidden = g.prefix[0] == '.';
            if (!env->entries) return COMP_NONE;
            env->entries(dir, gather, &g);
        }
    }
    if (r->ncand == 0) return COMP_NONE;
    if (r->ncand == 1 && r->total == 1) {
        size_t i = 0; while (g.full[0][i] && i + 1 < COMP_TEXT_MAX) { r->text[i] = g.full[0][i]; i++; } r->text[i] = '\0';
        r->append_space = !r->cand_dir[0] && !g.is_var;
        r->kind = COMP_UNIQUE;
        return r->kind;
    }
    /* several candidates: the longest start they all share */
    size_t common = slen(g.full[0]);
    for (size_t c = 1; c < r->ncand; c++) { size_t k = 0; while (k < common && g.full[c][k] == g.full[0][k]) k++; common = k; }
    if (common > wl) {
        for (size_t i = 0; i < common; i++) r->text[i] = g.full[0][i];
        r->text[common] = '\0';
        r->kind = COMP_PARTIAL;
    } else {
        for (size_t i = 0; i < wl; i++) r->text[i] = word[i];
        r->text[wl] = '\0';
        r->kind = COMP_LIST;
    }
    return r->kind;
}
