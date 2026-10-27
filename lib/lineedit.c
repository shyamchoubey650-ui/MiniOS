#include "include/lineedit.h"
#include "include/keyboard.h"

static size_t slen(const char *s) { size_t n = 0; while (s && s[n]) n++; return n; }

void edit_init(edit_t *e) { e->buf[0] = '\0'; e->len = e->pos = 0; }

bool edit_insert(edit_t *e, char c) {
    if ((unsigned char)c < ' ' || (unsigned char)c > 126) return false;
    if (e->len + 1 >= EDIT_MAX) return false;
    for (size_t i = e->len + 1; i > e->pos; i--) e->buf[i] = e->buf[i - 1];       /* shifts the NUL too */
    e->buf[e->pos++] = c; e->len++;
    return true;
}
bool edit_backspace(edit_t *e) {
    if (e->pos == 0) return false;
    for (size_t i = e->pos - 1; i < e->len; i++) e->buf[i] = e->buf[i + 1];
    e->pos--; e->len--;
    return true;
}
bool edit_delete(edit_t *e) {
    if (e->pos >= e->len) return false;
    for (size_t i = e->pos; i < e->len; i++) e->buf[i] = e->buf[i + 1];
    e->len--;
    return true;
}
bool edit_left(edit_t *e)  { if (e->pos == 0) return false; e->pos--; return true; }
bool edit_right(edit_t *e) { if (e->pos >= e->len) return false; e->pos++; return true; }
bool edit_home(edit_t *e)  { if (e->pos == 0) return false; e->pos = 0; return true; }
bool edit_end(edit_t *e)   { if (e->pos == e->len) return false; e->pos = e->len; return true; }
bool edit_kill_to_end(edit_t *e) { if (e->pos >= e->len) return false; e->buf[e->pos] = '\0'; e->len = e->pos; return true; }
bool edit_kill_line(edit_t *e) { if (e->len == 0) return false; e->buf[0] = '\0'; e->len = e->pos = 0; return true; }
bool edit_kill_word(edit_t *e) {
    size_t p = e->pos;
    while (p > 0 && e->buf[p - 1] == ' ') p--;
    while (p > 0 && e->buf[p - 1] != ' ') p--;
    if (p == e->pos) return false;
    size_t cut = e->pos - p;
    for (size_t i = p; i + cut <= e->len; i++) e->buf[i] = e->buf[i + cut];
    e->len -= cut; e->pos = p;
    return true;
}
bool edit_set(edit_t *e, const char *text) {
    size_t n = slen(text);
    if (n > EDIT_MAX - 1) n = EDIT_MAX - 1;
    for (size_t i = 0; i < n; i++) e->buf[i] = text[i];
    e->buf[n] = '\0'; e->len = e->pos = n;
    return true;
}
bool edit_replace(edit_t *e, size_t from, size_t to, const char *text) {
    size_t n = slen(text);
    if (from > to || to > e->len) return false;
    if (e->len - (to - from) + n + 1 > EDIT_MAX) return false;
    size_t tail = e->len - to;
    if (n > to - from) for (size_t i = tail + 1; i > 0; i--) e->buf[from + n + i - 1] = e->buf[to + i - 1];      /* grow: move the tail (and NUL) right, from the back */
    else for (size_t i = 0; i <= tail; i++) e->buf[from + n + i] = e->buf[to + i];                             /* shrink or same: move it left */
    for (size_t i = 0; i < n; i++) e->buf[from + i] = text[i];
    e->len = e->len - (to - from) + n; e->pos = from + n;
    return true;
}

size_t prompt_path(const char *cwd, const char *home, char *out, size_t cap) {
    char tmp[160]; size_t w = 0;
    if (!cap) return 0;
    if (!cwd || !*cwd) cwd = "?";
    size_t hl = slen(home), cl = slen(cwd);
    if (hl > 1 && home[0] == '/' && cl >= hl) {
        size_t i = 0; while (i < hl && cwd[i] == home[i]) i++;
        if (i == hl && (cwd[hl] == '\0' || cwd[hl] == '/')) { tmp[w++] = '~'; for (size_t k = hl; cwd[k] && w + 1 < sizeof tmp; k++) tmp[w++] = cwd[k]; tmp[w] = '\0'; cwd = tmp; cl = w; }
    }
    if (cl > 28) {                                                    /* keep the last two components */
        size_t slashes = 0, k = cl;
        while (k > 0) { if (cwd[k - 1] == '/' && ++slashes == 2) break; k--; }
        if (slashes == 2) {
            size_t o = 0; const char *dots = "...";
            for (size_t i = 0; dots[i] && o + 1 < cap; i++) out[o++] = dots[i];
            for (size_t i = k - 1; cwd[i] && o + 1 < cap; i++) out[o++] = cwd[i];
            out[o] = '\0';
            return o;
        }
    }
    size_t o = 0;
    for (size_t i = 0; cwd[i] && o + 1 < cap; i++) out[o++] = cwd[i];
    out[o] = '\0';
    return o;
}

size_t key_name(int code, char *out, size_t cap) {
    static const char *const special[] = { "Up", "Down", "Left", "Right", "Delete", "Home", "End", "PageUp", "PageDown", "Ctrl+Up(scroll)", "Ctrl+Down(scroll)" };
    char tmp[24]; size_t n = 0;
    if (code >= KEY_UP && code <= KEY_SCROLLDN) { const char *s = special[code - KEY_UP]; while (s[n]) { tmp[n] = s[n]; n++; } }
    else if (code == '\n') { const char *s = "Enter"; while (s[n]) { tmp[n] = s[n]; n++; } }
    else if (code == '\b') { const char *s = "Backspace"; while (s[n]) { tmp[n] = s[n]; n++; } }
    else if (code == '\t') { const char *s = "Tab"; while (s[n]) { tmp[n] = s[n]; n++; } }
    else if (code == 27)    { const char *s = "Esc"; while (s[n]) { tmp[n] = s[n]; n++; } }
    else if (code >= 1 && code <= 26) { const char *s = "Ctrl+"; while (s[n]) { tmp[n] = s[n]; n++; } tmp[n++] = (char)('A' + code - 1); }
    else if (code == ' ') { const char *s = "Space"; while (s[n]) { tmp[n] = s[n]; n++; } }
    else if (code > ' ' && code < 127) tmp[n++] = (char)code;
    else { const char *s = "?"; while (s[n]) { tmp[n] = s[n]; n++; } }
    size_t w = 0; if (cap == 0) return 0;
    for (size_t i = 0; i < n && w + 1 < cap; i++) out[w++] = tmp[i];
    out[w] = '\0';
    return w;
}
