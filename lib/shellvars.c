/* Shell variables and aliases; see include/shellvars.h. */
#include "include/shellvars.h"
#include "include/string.h"

typedef struct { bool used, exported; char name[VAR_NAME_MAX + 1]; char value[VAR_VALUE_MAX + 1]; } var_t;
typedef struct { bool used; char name[VAR_NAME_MAX + 1]; char value[VAR_VALUE_MAX + 1]; } alias_t;

static var_t   vars[VAR_MAX];
static alias_t aliases[ALIAS_MAX];
static int     last_status;
void sv_set_status(int st) { last_status = st; }

void sv_reset(void) { memset(vars, 0, sizeof vars); memset(aliases, 0, sizeof aliases); last_status = 0; }

bool sv_name_valid(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > VAR_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (i > 0 && c >= '0' && c <= '9');
        if (!ok) return false;
    }
    return true;
}

static void copy(char *dst, size_t cap, const char *src) { size_t i = 0; while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; } dst[i] = '\0'; }

static var_t *find_var(const char *name) { for (size_t i = 0; i < VAR_MAX; i++) if (vars[i].used && !strcmp(vars[i].name, name)) return &vars[i]; return NULL; }

int var_set(const char *name, const char *value, bool exported) {
    if (!sv_name_valid(name)) return SV_ERR_NAME;
    if (strlen(value) > VAR_VALUE_MAX) return SV_ERR_VALUE;
    var_t *v = find_var(name);
    if (!v) { for (size_t i = 0; i < VAR_MAX; i++) if (!vars[i].used) { v = &vars[i]; break; } if (!v) return SV_ERR_FULL; memset(v, 0, sizeof *v); v->used = true; copy(v->name, sizeof v->name, name); }
    copy(v->value, sizeof v->value, value);
    if (exported) v->exported = true;
    return SV_OK;
}

int var_unset(const char *name) { var_t *v = find_var(name); if (!v) return SV_ERR_NOT_FOUND; memset(v, 0, sizeof *v); return SV_OK; }
const char *var_get(const char *name) { var_t *v = name ? find_var(name) : NULL; return v ? v->value : NULL; }
int var_export(const char *name) { var_t *v = find_var(name); if (!v) return SV_ERR_NOT_FOUND; v->exported = true; return SV_OK; }
size_t var_count(void) { size_t n = 0; for (size_t i = 0; i < VAR_MAX; i++) n += vars[i].used; return n; }

/* k-th used entry in name order (selection by rank: tables are tiny) */
static var_t *var_ranked(size_t k) {
    for (size_t i = 0; i < VAR_MAX; i++) {
        if (!vars[i].used) continue;
        size_t before = 0;
        for (size_t j = 0; j < VAR_MAX; j++) if (vars[j].used && strcmp(vars[j].name, vars[i].name) < 0) before++;
        if (before == k) return &vars[i];
    }
    return NULL;
}
bool var_at(size_t k, const char **name, const char **value, bool *exported) {
    var_t *v = var_ranked(k);
    if (!v) return false;
    if (name) *name = v->name;
    if (value) *value = v->value;
    if (exported) *exported = v->exported;
    return true;
}

/* ---------- aliases ---------- */
static alias_t *find_alias(const char *name) { for (size_t i = 0; i < ALIAS_MAX; i++) if (aliases[i].used && !strcmp(aliases[i].name, name)) return &aliases[i]; return NULL; }

int alias_set(const char *name, const char *value) {
    if (!sv_name_valid(name)) return SV_ERR_NAME;
    if (strlen(value) > VAR_VALUE_MAX || value[0] == '\0') return SV_ERR_VALUE;
    alias_t *a = find_alias(name);
    if (!a) { for (size_t i = 0; i < ALIAS_MAX; i++) if (!aliases[i].used) { a = &aliases[i]; break; } if (!a) return SV_ERR_FULL; memset(a, 0, sizeof *a); a->used = true; copy(a->name, sizeof a->name, name); }
    copy(a->value, sizeof a->value, value);
    return SV_OK;
}
int alias_remove(const char *name) { alias_t *a = find_alias(name); if (!a) return SV_ERR_NOT_FOUND; memset(a, 0, sizeof *a); return SV_OK; }
const char *alias_get(const char *name) { alias_t *a = name ? find_alias(name) : NULL; return a ? a->value : NULL; }
size_t alias_count(void) { size_t n = 0; for (size_t i = 0; i < ALIAS_MAX; i++) n += aliases[i].used; return n; }
bool alias_at(size_t k, const char **name, const char **value) {
    for (size_t i = 0; i < ALIAS_MAX; i++) {
        if (!aliases[i].used) continue;
        size_t before = 0;
        for (size_t j = 0; j < ALIAS_MAX; j++) if (aliases[j].used && strcmp(aliases[j].name, aliases[i].name) < 0) before++;
        if (before == k) { if (name) *name = aliases[i].name; if (value) *value = aliases[i].value; return true; }
    }
    return false;
}

/* ---------- expansion ---------- */
int sv_expand_vars(const char *in, char *out, size_t cap) {
    size_t o = 0; char quote = 0;
    for (size_t i = 0; in[i]; ) {
        char c = in[i];
        if (quote) { if (c == quote) quote = 0; }
        else if (c == '\'' || c == '"') quote = c;
        if (c == '$' && quote != '\'') {
            char name[VAR_NAME_MAX + 2]; size_t n = 0; size_t j = i + 1; bool braces = false;
            if (in[j] == '{') { braces = true; j++; }
            if (in[j] == '?') { name[n++] = '?'; j++; }
            else while (n < VAR_NAME_MAX + 1 && (in[j] == '_' || (in[j] >= 'a' && in[j] <= 'z') || (in[j] >= 'A' && in[j] <= 'Z') || (n > 0 && in[j] >= '0' && in[j] <= '9'))) name[n++] = in[j++];
            name[n] = '\0';
            if (braces) { if (in[j] != '}') n = 0; else j++; }
            if (n > 0 && n <= VAR_NAME_MAX) {
                char num[12]; const char *v;
                if (name[0] == '?') { unsigned u = (unsigned)(last_status < 0 ? -last_status : last_status); size_t k = 0; char t[12]; do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u); size_t m = 0; if (last_status < 0) num[m++] = '-'; while (k) num[m++] = t[--k]; num[m] = '\0'; v = num; }
                else v = var_get(name);
                if (v) { size_t l = strlen(v); if (o + l + 1 > cap) return SV_ERR_SPACE; memcpy(out + o, v, l); o += l; }
                i = j;
                continue;
            }
        }
        if (o + 2 > cap) return SV_ERR_SPACE;
        out[o++] = c; i++;
    }
    if (o + 1 > cap) return SV_ERR_SPACE;
    out[o] = '\0';
    return SV_OK;
}

int sv_expand_alias(const char *in, char *out, size_t cap, bool *changed) {
    size_t ws = 0; while (in[ws] == ' ' || in[ws] == '\t') ws++;
    size_t we = ws; while (in[we] && in[we] != ' ' && in[we] != '\t') we++;
    char word[VAR_NAME_MAX + 2]; bool use = false;
    if (we - ws > 0 && we - ws <= VAR_NAME_MAX) { memcpy(word, in + ws, we - ws); word[we - ws] = '\0'; use = alias_get(word) != NULL; }
    if (changed) *changed = use;
    size_t o = 0;
    if (use) {
        const char *v = alias_get(word); size_t l = strlen(v);
        /* an alias whose first word is its own name (alias ls="ls -l") runs the real command: expansion happens once */
        if (ws + l + strlen(in + we) + 1 > cap) return SV_ERR_SPACE;
        memcpy(out, in, ws); o = ws; memcpy(out + o, v, l); o += l;
        size_t rest = strlen(in + we); memcpy(out + o, in + we, rest); o += rest;
    } else { size_t l = strlen(in); if (l + 1 > cap) return SV_ERR_SPACE; memcpy(out, in, l); o = l; }
    out[o] = '\0';
    return SV_OK;
}
