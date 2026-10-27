#include "include/cmdutil.h"
#include "include/screen.h"
#include "include/filesystem.h"
#include "include/syscall.h"
#include "include/numparse.h"
#include "include/string.h"

void cu_color(uint8_t fg) { screen_set_color(fg, VGA_BLACK); }

static void err_prefix(void) { cu_color(VGA_LIGHT_RED); screen_puts("Error: "); cu_color(VGA_LIGHT_GREY); }

int cu_usage(const char *text) { screen_puts("usage: "); screen_puts(text); screen_putc('\n'); return 1; }

int cu_error(const char *cmd, const char *msg) {
    err_prefix(); screen_puts(cmd); screen_puts(": "); screen_puts(msg); screen_putc('\n');
    return 1;
}

int cu_fs_error(const char *cmd, const char *path, int rc, int kind) {
    err_prefix();
    switch (rc) {
        case FS_ERR_NOT_FOUND:
            if (kind == CU_DIR)       { screen_puts("directory '"); screen_puts(path); screen_puts("' does not exist."); }
            else if (kind == CU_FILE) { screen_puts("file '"); screen_puts(path); screen_puts("' not found."); }
            else                      { screen_putc('\''); screen_puts(path); screen_puts("' does not exist."); }
            break;
        case FS_ERR_NOT_DIR:   screen_putc('\''); screen_puts(path); screen_puts("' is not a directory."); break;
        case FS_ERR_IS_DIR:    screen_putc('\''); screen_puts(path); screen_puts("' is a directory."); break;
        case FS_ERR_EXISTS:    screen_putc('\''); screen_puts(path); screen_puts("' already exists."); break;
        case FS_ERR_NOT_EMPTY: screen_puts("directory '"); screen_puts(path); screen_puts("' is not empty."); break;
        case FS_ERR_LOCKED:    screen_putc('\''); screen_puts(path); screen_puts("' is locked by another user (only they or root can change it)."); break;
        case FS_ERR_PROTECTED: screen_putc('\''); screen_puts(path); screen_puts("' is protected (run 'unprotect' first)."); break;
        case FS_ERR_PERMISSION: screen_puts("permission denied: '"); screen_puts(path); screen_puts("'."); break;
        default:
            screen_puts(cmd); screen_puts(": '"); screen_puts(path); screen_puts("': "); screen_puts(sys_strerror(rc));
            break;
    }
    screen_putc('\n');
    return 1;
}

void cu_pad(const char *s, size_t w) { screen_puts(s); for (size_t k = strlen(s); k < w; k++) screen_putc(' '); }

void cu_num_pad(uint32_t v, size_t w) {
    char t[12]; size_t n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (size_t k = n; k < w; k++) screen_putc(' ');
    while (n) screen_putc(t[--n]);
}

void cu_size_str(char *buf, uint32_t b) {
    char tmp[12]; size_t n = 0, w = 0; uint32_t whole, tenth = 0; const char *unit;
    if (b >= (1u << 20))      { whole = b >> 20; tenth = ((b & 0xFFFFFu) * 10) >> 20; unit = " MiB"; }
    else if (b >= 1024)       { whole = b >> 10; tenth = ((b & 1023u) * 10) >> 10; unit = " KiB"; }
    else                      { whole = b; unit = " B"; }
    do { tmp[n++] = (char)('0' + whole % 10); whole /= 10; } while (whole);
    while (n) buf[w++] = tmp[--n];
    if (b >= 1024) { buf[w++] = '.'; buf[w++] = (char)('0' + tenth); }
    for (const char *u = unit; *u; u++) buf[w++] = *u;
    buf[w] = '\0';
}
void cu_put_size(uint32_t b) { char t[16]; cu_size_str(t, b); screen_puts(t); }

void cu_print_text(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        screen_putc((c == '\n' || c == '\t' || (c >= 32 && c < 127)) ? (char)c : '.');
    }
}

bool cu_parse_count(const char *s, uint32_t *out, uint32_t max) {
    uint32_t v;
    if (!parse_u32(s, &v) || v > max) return false;
    *out = v;
    return true;
}

static const char *pipe_text; static size_t pipe_len; static bool pipe_on;
void cu_pipe_set(const char *buf, size_t len) { pipe_text = buf; pipe_len = len; pipe_on = true; }
void cu_pipe_clear(void) { pipe_text = NULL; pipe_len = 0; pipe_on = false; }
bool cu_pipe_active(void) { return pipe_on; }

bool cu_load_file(const char *cmd, const char *path, char **buf, size_t *len) {
    if (path[0] == '-' && path[1] == '\0') {                       /* standard input: the output of the previous command in a pipeline */
        if (!pipe_on) { cu_error(cmd, "no input: '-' stands for the output of the previous command (use it as: cmd1 | cmd2)"); return false; }
        char *b = sys_memory_alloc(pipe_len + 1);
        if (!b) { cu_error(cmd, "out of memory"); return false; }
        for (size_t i = 0; i < pipe_len; i++) b[i] = pipe_text[i];
        b[pipe_len] = '\0';
        *buf = b; *len = pipe_len;
        return true;
    }
    fs_stat_t st;
    int rc = sys_stat(path, &st);
    if (rc != 0) { cu_fs_error(cmd, path, rc, CU_FILE); return false; }
    if (st.type == FS_DIR) { cu_fs_error(cmd, path, FS_ERR_IS_DIR, CU_FILE); return false; }
    int fd = sys_open(path, SYS_O_READ);
    if (fd < 0) { cu_fs_error(cmd, path, fd, CU_FILE); return false; }
    char *b = sys_memory_alloc(st.size + 1);
    if (!b) { sys_close(fd); cu_error(cmd, "out of memory"); return false; }
    size_t got = 0;
    while (got < st.size) {                                  /* read until the size we saw, or EOF */
        int n = sys_read(fd, b + got, st.size - got);
        if (n < 0) { sys_close(fd); sys_memory_free(b); cu_fs_error(cmd, path, n, CU_FILE); return false; }
        if (n == 0) break;
        got += (size_t)n;
    }
    sys_close(fd);
    b[got] = '\0';
    *buf = b; *len = got;
    return true;
}

void cu_free_file(char *buf) { sys_memory_free(buf); }

void cu_wipe(void *p, size_t n) { volatile uint8_t *v = p; while (n--) *v++ = 0; }

bool cu_read_secret(const char *prompt, char *buf, size_t cap) {
    size_t n = 0; bool ok = true;
    screen_puts(prompt);
    for (;;) {
        char c;
        if (sys_read(0, &c, 1) != 1) { ok = false; break; }
        if (c == '\n' || c == '\r') break;
        if (c == 3) { ok = false; break; }                          /* Ctrl+C */
        if (c == '\b') { if (n) { n--; screen_backspace(); } continue; }
        if ((unsigned char)c < 32 || (unsigned char)c > 126) continue;
        if (n + 1 >= cap) { ok = false; continue; }                 /* too long: keep reading to the end of the line */
        buf[n++] = c; screen_putc('*');
    }
    buf[n] = '\0';
    screen_putc('\n');
    if (!ok) cu_wipe(buf, cap);
    return ok;
}

void cu_uid_name(uint32_t uid, char *out, size_t cap) {
    user_info_t u;
    if (sys_user_lookup(NULL, (int32_t)uid, &u) == 0) { size_t i = 0; while (u.name[i] && i + 1 < cap) { out[i] = u.name[i]; i++; } out[i] = '\0'; return; }
    char t[12]; size_t k = 0, w = 0;
    do { t[k++] = (char)('0' + uid % 10); uid /= 10; } while (uid);
    while (k && w + 1 < cap) out[w++] = t[--k];
    out[w] = '\0';
}

void cu_sleep_ms(uint32_t ms) {
#ifndef MINIOS_HOST_TEST            /* host tests have no running scheduler to sleep on */
    sys_sleep(ms);
#else
    (void)ms;
#endif
}
void cu_auth_delay(void) { cu_sleep_ms(1000); }

#include "include/mathlib.h"
void cu_put_u64(uint64_t v) { char b[24]; math_to_base(v, 10, b, sizeof b); screen_puts(b); }
void cu_put_i64(int64_t v) { if (v < 0) { screen_putc('-'); cu_put_u64(0 - (uint64_t)v); } else cu_put_u64((uint64_t)v); }
