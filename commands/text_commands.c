/* Text and viewing commands (Phase 8): cat head tail wc grep hexdump cmp motd.
 * grep matches plain substrings (NOT regular expressions). */
#include "include/builtins.h"
#include "include/cmdutil.h"
#include "include/screen.h"
#include "include/filesystem.h"
#include "include/textutil.h"
#include "include/syscall.h"
#include "include/numparse.h"
#include "include/string.h"

static int cmd_cat(int argc, const char **argv) {
    if (argc < 2) return cu_usage("cat <file...>");
    int status = 0;
    for (int i = 1; i < argc; i++) {
        char *b; size_t n;
        if (!cu_load_file("cat", argv[i], &b, &n)) { status = 1; continue; }
        cu_print_text(b, n);
        if (n && b[n - 1] != '\n') screen_putc('\n');
        cu_free_file(b);
    }
    return status;
}

/* head/tail share option parsing: [-n N] <file> */
static bool parse_n_file(const char *cmd, int argc, const char **argv, uint32_t *n, const char **file) {
    *n = 10; *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) {
            if (i + 1 >= argc || !cu_parse_count(argv[i + 1], n, 1000000)) { cu_error(cmd, "-n needs a line count"); return false; }
            i++;
        } else if (!*file) *file = argv[i];
        else { cu_usage(cmd[0] == 'h' ? "head [-n N] <file>" : "tail [-n N] <file>"); return false; }
    }
    if (!*file) { cu_usage(cmd[0] == 'h' ? "head [-n N] <file>" : "tail [-n N] <file>"); return false; }
    return true;
}

static int cmd_head(int argc, const char **argv) {
    uint32_t n; const char *f; char *b; size_t len;
    if (!parse_n_file("head", argc, argv, &n, &f) || !cu_load_file("head", f, &b, &len)) return 1;
    size_t k = text_head_len(b, len, n);
    cu_print_text(b, k); if (k && b[k - 1] != '\n') screen_putc('\n');
    cu_free_file(b);
    return 0;
}
static int cmd_tail(int argc, const char **argv) {
    uint32_t n; const char *f; char *b; size_t len;
    if (!parse_n_file("tail", argc, argv, &n, &f) || !cu_load_file("tail", f, &b, &len)) return 1;
    size_t s = text_tail_start(b, len, n);
    cu_print_text(b + s, len - s); if (len > s && b[len - 1] != '\n') screen_putc('\n');
    cu_free_file(b);
    return 0;
}

static int cmd_wc(int argc, const char **argv) {
    bool l = false, w = false, c = false; int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        for (const char *o = argv[first] + 1; *o; o++) { if (*o == 'l') l = true; else if (*o == 'w') w = true; else if (*o == 'c') c = true; else return cu_error("wc", "options are -l -w -c"); }
    }
    if (first >= argc) return cu_usage("wc [-l] [-w] [-c] <file...>");
    if (!l && !w && !c) l = w = c = true;
    int status = 0; size_t tl = 0, tw = 0, tc = 0; int files = 0;
    for (int i = first; i < argc; i++) {
        char *b; size_t len, lines, words, bytes;
        if (!cu_load_file("wc", argv[i], &b, &len)) { status = 1; continue; }
        text_counts(b, len, &lines, &words, &bytes); cu_free_file(b);
        if (l) cu_num_pad((uint32_t)lines, 7);
        if (w) cu_num_pad((uint32_t)words, 7);
        if (c) cu_num_pad((uint32_t)bytes, 7);
        if (strcmp(argv[i], "-")) { screen_putc(' '); screen_puts(argv[i]); }       /* piped input has no name */
        screen_putc('\n');
        tl += lines; tw += words; tc += bytes; files++;
    }
    if (files > 1) {
        if (l) cu_num_pad((uint32_t)tl, 7);
        if (w) cu_num_pad((uint32_t)tw, 7);
        if (c) cu_num_pad((uint32_t)tc, 7);
        screen_puts(" total\n");
    }
    return status;
}

static int cmd_grep(int argc, const char **argv) {
    bool number = false, icase = false; int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        for (const char *o = argv[first] + 1; *o; o++) { if (*o == 'n') number = true; else if (*o == 'i') icase = true; else return cu_error("grep", "options are -n -i"); }
    }
    if (argc - first < 2) return cu_usage("grep [-n] [-i] <text> <file...>   (plain text, not a regular expression)");
    const char *pat = argv[first]; int nfiles = argc - first - 1, matches = 0, status = 0;
    for (int i = first + 1; i < argc; i++) {
        char *b; size_t len;
        if (!cu_load_file("grep", argv[i], &b, &len)) { status = 2; continue; }
        size_t pos = 0; uint32_t lineno = 0;
        while (pos < len) {
            size_t end = pos; while (end < len && b[end] != '\n') end++;
            lineno++;
            if (text_contains(b + pos, end - pos, pat, icase)) {
                matches++;
                if (nfiles > 1) { screen_puts(argv[i]); screen_putc(':'); }
                if (number) { screen_put_dec(lineno); screen_putc(':'); }
                cu_print_text(b + pos, end - pos); screen_putc('\n');
            }
            pos = end + 1;
        }
        cu_free_file(b);
    }
    return status ? status : (matches ? 0 : 1);
}

static void hexbyte(uint8_t v) { static const char d[] = "0123456789ABCDEF"; screen_putc(d[v >> 4]); screen_putc(d[v & 15]); }

static int cmd_hexdump(int argc, const char **argv) {
    uint32_t limit = 128;
    if (argc < 2 || argc > 3) return cu_usage("hexdump <file> [bytes<=1024]");
    if (argc == 3 && (!cu_parse_count(argv[2], &limit, 1024) || limit == 0)) return cu_error("hexdump", "byte count must be 1 to 1024");
    char *b; size_t len;
    if (!cu_load_file("hexdump", argv[1], &b, &len)) return 1;
    size_t n = len < limit ? len : limit;
    for (size_t off = 0; off < n; off += 16) {
        char a[11]; for (int k = 0; k < 8; k++) a[k] = "0123456789ABCDEF"[((uint32_t)off >> (28 - 4 * k)) & 15]; a[8] = ' '; a[9] = ' '; a[10] = '\0';
        screen_puts(a);
        for (size_t k = 0; k < 16; k++) { if (off + k < n) { hexbyte((uint8_t)b[off + k]); screen_putc(' '); } else screen_puts("   "); }
        screen_putc('|');
        for (size_t k = 0; k < 16 && off + k < n; k++) { uint8_t c = (uint8_t)b[off + k]; screen_putc(c >= 32 && c < 127 ? (char)c : '.'); }
        screen_puts("|\n");
    }
    if (len > n) { screen_puts("... "); screen_put_dec((uint32_t)(len - n)); screen_puts(" more byte(s)\n"); }
    cu_free_file(b);
    return 0;
}

static int cmd_cmp(int argc, const char **argv) {
    if (argc != 3) return cu_usage("cmp <file1> <file2>");
    char *a, *b; size_t la, lb;
    if (!cu_load_file("cmp", argv[1], &a, &la)) return 2;
    if (!cu_load_file("cmp", argv[2], &b, &lb)) { cu_free_file(a); return 2; }
    size_t i = 0, m = la < lb ? la : lb;
    while (i < m && a[i] == b[i]) i++;
    int status = 0;
    if (i == m && la == lb) screen_puts("files are identical\n");
    else if (i == m) { screen_puts(la < lb ? argv[1] : argv[2]); screen_puts(" is shorter: identical for the first "); screen_put_dec((uint32_t)m); screen_puts(" byte(s)\n"); status = 1; }
    else { screen_puts("files differ at byte "); screen_put_dec((uint32_t)(i + 1)); screen_puts(": "); hexbyte((uint8_t)a[i]); screen_puts(" vs "); hexbyte((uint8_t)b[i]); screen_putc('\n'); status = 1; }
    cu_free_file(a); cu_free_file(b);
    return status;
}

static int cmd_motd(int argc, const char **argv) {
    (void)argc; (void)argv;
    const char *f[2] = { "cat", "/etc/motd" };
    return cmd_cat(2, f);
}

static const struct { const char *name; cmd_handler_t fn; } bindings[] = {
    { "cat", cmd_cat }, { "head", cmd_head }, { "tail", cmd_tail }, { "wc", cmd_wc }, { "grep", cmd_grep },
    { "hexdump", cmd_hexdump }, { "cmp", cmd_cmp }, { "motd", cmd_motd },
};

int text_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
