/* Text utilities (Phase 11): sort uniq cut tr rev nl tac fold upper lower capitalize sum min max avg join split paste
 * strlen repeatstr trim replace contains startswith endswith ascii wordfreq.
 * File commands read a file with cu_load_file (system calls) and print to the screen, so they work with `> file` redirection.
 * Since Phase 13 they also read a pipe: `cat names.txt | sort | uniq -c` (see include/pipeline.h). Limits: 4096 lines per file, ASCII only. */
#include "include/builtins.h"
#include "include/cmdutil.h"
#include "include/screen.h"
#include "include/syscall.h"
#include "include/mathlib.h"
#include "include/numparse.h"
#include "include/string.h"

#define MAX_LINES 4096

typedef struct { char *buf; char **line; size_t count; } lines_t;

/* Loads a file and splits it in place: every line becomes a NUL-terminated string (CR removed). */
static bool lines_load(const char *cmd, const char *path, lines_t *L) {
    size_t len;
    L->line = NULL; L->count = 0;
    if (!cu_load_file(cmd, path, &L->buf, &len)) return false;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) if (L->buf[i] == '\n') n++;
    if (len && L->buf[len - 1] != '\n') n++;
    if (n > MAX_LINES) { cu_free_file(L->buf); cu_error(cmd, "file has more than 4096 lines."); return false; }
    L->line = sys_memory_alloc((n ? n : 1) * sizeof(char *));
    if (!L->line) { cu_free_file(L->buf); cu_error(cmd, "out of memory"); return false; }
    size_t pos = 0;
    while (pos < len) {
        size_t e = pos; while (e < len && L->buf[e] != '\n') e++;
        L->line[L->count++] = L->buf + pos;
        size_t end = e; if (end > pos && L->buf[end - 1] == '\r') end--;
        L->buf[end] = '\0';
        if (e < len) L->buf[e] = '\0';
        pos = e + 1;
    }
    return true;
}
static void lines_free(lines_t *L) { if (L->line) sys_memory_free(L->line); if (L->buf) cu_free_file(L->buf); }

static void put_line(const char *s) { screen_puts(s); screen_putc('\n'); }

/* Joins argv[from..] with single spaces into a static buffer, or reads the file after -f. */
static bool text_arg(const char *cmd, int argc, const char **argv, int from, char *out, size_t cap, bool *from_file, lines_t *L) {
    *from_file = false;
    if (argc > from + 1 && !strcmp(argv[from], "-f")) {
        if (argc != from + 2) { cu_usage("(see help)"); return false; }
        if (!lines_load(cmd, argv[from + 1], L)) return false;
        *from_file = true;
        return true;
    }
    size_t o = 0;
    for (int i = from; i < argc; i++) {
        size_t l = strlen(argv[i]);
        if (o + l + 2 > cap) { cu_error(cmd, "text too long."); return false; }
        if (i > from) out[o++] = ' ';
        memcpy(out + o, argv[i], l); o += l;
    }
    out[o] = '\0';
    return true;
}

typedef void (*line_fn)(const char *in, char *out, size_t cap);

/* upper/lower/capitalize/trim: transform a text, or every line of a file (-f). */
static int map_text(const char *cmd, const char *usage, int argc, const char **argv, line_fn fn) {
    if (argc < 2) return cu_usage(usage);
    char text[256], res[300]; bool ff; lines_t L = { 0, 0, 0 };
    if (!text_arg(cmd, argc, argv, 1, text, sizeof text, &ff, &L)) return 1;
    if (ff) { for (size_t i = 0; i < L.count; i++) { char tmp[300]; size_t l = strlen(L.line[i]); if (l >= sizeof tmp) l = sizeof tmp - 1; memcpy(tmp, L.line[i], l); tmp[l] = '\0'; fn(tmp, res, sizeof res); put_line(res); } lines_free(&L); }
    else { fn(text, res, sizeof res); put_line(res); }
    return 0;
}
static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static char lo(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static void f_upper(const char *in, char *out, size_t cap) { size_t i = 0; for (; in[i] && i + 1 < cap; i++) out[i] = up(in[i]); out[i] = '\0'; }
static void f_lower(const char *in, char *out, size_t cap) { size_t i = 0; for (; in[i] && i + 1 < cap; i++) out[i] = lo(in[i]); out[i] = '\0'; }
static void f_cap(const char *in, char *out, size_t cap) {
    bool start = true; size_t i = 0;
    for (; in[i] && i + 1 < cap; i++) { out[i] = start ? up(in[i]) : lo(in[i]); start = in[i] == ' ' || in[i] == '\t'; }
    out[i] = '\0';
}
static void f_trim(const char *in, char *out, size_t cap) {
    size_t s = 0, e = strlen(in);
    while (in[s] == ' ' || in[s] == '\t') s++;
    while (e > s && (in[e - 1] == ' ' || in[e - 1] == '\t')) e--;
    size_t n = e - s; if (n + 1 > cap) n = cap - 1;
    memcpy(out, in + s, n); out[n] = '\0';
}
static int cmd_upper(int argc, const char **argv) { return map_text("upper", "upper <text...> | upper -f <file>", argc, argv, f_upper); }
static int cmd_lower(int argc, const char **argv) { return map_text("lower", "lower <text...> | lower -f <file>", argc, argv, f_lower); }
static int cmd_capitalize(int argc, const char **argv) { return map_text("capitalize", "capitalize <text...> | capitalize -f <file>", argc, argv, f_cap); }
static int cmd_trim(int argc, const char **argv) { return map_text("trim", "trim <text...>     (removes spaces and tabs at both ends)", argc, argv, f_trim); }

/* ---------- sort ---------- */
static int64_t lead_num(const char *s) {                     /* leading integer, 0 if none (for sort -n) */
    while (*s == ' ' || *s == '\t') s++;
    bool neg = *s == '-'; if (neg || *s == '+') s++;
    int64_t v = 0; while (*s >= '0' && *s <= '9' && v < INT64_MAX / 20) v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}
static int cmp_lines(const char *a, const char *b, bool numeric) {
    if (numeric) { int64_t x = lead_num(a), y = lead_num(b); if (x != y) return x < y ? -1 : 1; }
    return strcmp(a, b);
}
static void merge_sort(char **a, char **tmp, size_t n, bool numeric) {   /* stable */
    if (n < 2) return;
    size_t m = n / 2;
    merge_sort(a, tmp, m, numeric); merge_sort(a + m, tmp, n - m, numeric);
    size_t i = 0, j = m, k = 0;
    while (i < m && j < n) tmp[k++] = cmp_lines(a[j], a[i], numeric) < 0 ? a[j++] : a[i++];
    while (i < m) tmp[k++] = a[i++];
    while (j < n) tmp[k++] = a[j++];
    memcpy(a, tmp, n * sizeof(char *));
}
static int cmd_sort(int argc, const char **argv) {
    bool rev = false, num = false, uniq = false; const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] && !file) { for (const char *f = argv[i] + 1; *f; f++) { if (*f == 'r') rev = true; else if (*f == 'n') num = true; else if (*f == 'u') uniq = true; else return cu_usage("sort [-r] [-n] [-u] <file>"); } }
        else if (!file) file = argv[i]; else return cu_usage("sort [-r] [-n] [-u] <file>");
    }
    if (!file) return cu_usage("sort [-r] [-n] [-u] <file>     (-r reverse, -n numeric, -u drop duplicates)");
    lines_t L; if (!lines_load("sort", file, &L)) return 1;
    char **tmp = sys_memory_alloc((L.count ? L.count : 1) * sizeof(char *));
    if (!tmp) { lines_free(&L); return cu_error("sort", "out of memory"); }
    merge_sort(L.line, tmp, L.count, num);
    for (size_t k = 0; k < L.count; k++) {
        size_t i = rev ? L.count - 1 - k : k;
        if (uniq && k > 0) { size_t p = rev ? L.count - k : k - 1; if (cmp_lines(L.line[i], L.line[p], num) == 0 && strcmp(L.line[i], L.line[p]) == 0) continue; }
        put_line(L.line[i]);
    }
    sys_memory_free(tmp); lines_free(&L);
    return 0;
}

static int cmd_uniq(int argc, const char **argv) {
    bool count = argc == 3 && !strcmp(argv[1], "-c");
    if (!(argc == 2 || count)) return cu_usage("uniq [-c] <file>     (removes ADJACENT duplicates; sort first for all)");
    lines_t L; if (!lines_load("uniq", argv[argc - 1], &L)) return 1;
    for (size_t i = 0; i < L.count; ) {
        size_t j = i + 1; while (j < L.count && !strcmp(L.line[i], L.line[j])) j++;
        if (count) { cu_num_pad((uint32_t)(j - i), 6); screen_putc(' '); }
        put_line(L.line[i]);
        i = j;
    }
    lines_free(&L);
    return 0;
}

static int cmd_tac(int argc, const char **argv) {
    if (argc != 2) return cu_usage("tac <file>     (prints the lines in reverse order)");
    lines_t L; if (!lines_load("tac", argv[1], &L)) return 1;
    for (size_t i = L.count; i > 0; i--) put_line(L.line[i - 1]);
    lines_free(&L);
    return 0;
}
static int cmd_rev(int argc, const char **argv) {
    if (argc != 2) return cu_usage("rev <file>     (reverses the characters of each line)");
    lines_t L; if (!lines_load("rev", argv[1], &L)) return 1;
    for (size_t i = 0; i < L.count; i++) { size_t l = strlen(L.line[i]); for (size_t k = l; k > 0; k--) screen_putc(L.line[i][k - 1]); screen_putc('\n'); }
    lines_free(&L);
    return 0;
}
static int cmd_nl(int argc, const char **argv) {
    if (argc != 2) return cu_usage("nl <file>     (numbers the lines)");
    lines_t L; if (!lines_load("nl", argv[1], &L)) return 1;
    for (size_t i = 0; i < L.count; i++) { cu_num_pad((uint32_t)(i + 1), 6); screen_puts("  "); put_line(L.line[i]); }
    lines_free(&L);
    return 0;
}
static int cmd_fold(int argc, const char **argv) {
    uint32_t w = 40; const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w")) { if (i + 1 >= argc || !cu_parse_count(argv[i + 1], &w, 200) || w < 4) return cu_error("fold", "-w needs a width between 4 and 200."); i++; }
        else if (!file) file = argv[i]; else return cu_usage("fold [-w N] <file>");
    }
    if (!file) return cu_usage("fold [-w N] <file>     (wraps long lines at N characters, default 40)");
    lines_t L; if (!lines_load("fold", file, &L)) return 1;
    for (size_t i = 0; i < L.count; i++) {
        const char *s = L.line[i]; size_t l = strlen(s);
        if (l == 0) { screen_putc('\n'); continue; }
        for (size_t p = 0; p < l; p += w) { for (size_t k = p; k < l && k < p + w; k++) screen_putc(s[k]); screen_putc('\n'); }
    }
    lines_free(&L);
    return 0;
}

/* ---------- cut, tr, join, split, paste ---------- */
static int cmd_cut(int argc, const char **argv) {
    char delim = ' '; uint32_t f1 = 0, f2 = 0; const char *file = NULL; bool have_f = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d")) { if (i + 1 >= argc || strlen(argv[i + 1]) != 1) return cu_error("cut", "-d needs ONE character."); delim = argv[++i][0]; }
        else if (!strcmp(argv[i], "-f")) {
            if (i + 1 >= argc) return cu_error("cut", "-f needs a field number such as 2 or 1,3.");
            const char *a = argv[++i]; char first[12]; size_t k = 0;
            while (a[k] && a[k] != ',' && k < 11) { first[k] = a[k]; k++; } first[k] = '\0';
            if (!cu_parse_count(first, &f1, 1000) || f1 == 0) return cu_error("cut", "fields count from 1.");
            if (a[k] == ',') { if (!cu_parse_count(a + k + 1, &f2, 1000) || f2 == 0) return cu_error("cut", "fields count from 1."); }
            have_f = true;
        } else if (!file) file = argv[i]; else return cu_usage("cut -d <char> -f <N[,M]> <file>");
    }
    if (!have_f || !file) return cu_usage("cut -d <char> -f <N[,M]> <file>     (default delimiter: a space; fields count from 1)");
    lines_t L; if (!lines_load("cut", file, &L)) return 1;
    for (size_t i = 0; i < L.count; i++) {
        const char *s = L.line[i]; uint32_t field = 1; const char *start = s; bool first_out = true;
        for (const char *p = s; ; p++) {
            if (*p == delim || *p == '\0') {
                if (field == f1 || (f2 && field == f2)) { if (!first_out) screen_putc(delim); for (const char *q = start; q < p; q++) screen_putc(*q); first_out = false; }
                field++; start = p + 1;
                if (*p == '\0') break;
            }
        }
        screen_putc('\n');
    }
    lines_free(&L);
    return 0;
}

static size_t expand_set(const char *s, char *out, size_t cap) {      /* a-z ranges */
    size_t n = 0;
    for (size_t i = 0; s[i] && n < cap; i++) {
        if (s[i + 1] == '-' && s[i + 2] && s[i + 2] >= s[i]) { for (int c = (unsigned char)s[i]; c <= (unsigned char)s[i + 2] && n < cap; c++) out[n++] = (char)c; i += 2; }
        else out[n++] = s[i];
    }
    return n;
}
static int cmd_tr(int argc, const char **argv) {
    bool del = argc >= 2 && !strcmp(argv[1], "-d");
    if (!((del && argc == 4) || (!del && argc == 4))) return cu_usage("tr <from> <to> <file>  |  tr -d <chars> <file>     (ranges like a-z allowed)");
    char from[130], to[130]; size_t nf, nt = 0;
    nf = expand_set(del ? argv[2] : argv[1], from, 128);
    if (!del) nt = expand_set(argv[2], to, 128);
    if (nf == 0 || (!del && nt == 0)) return cu_error("tr", "empty character set.");
    lines_t L; if (!lines_load("tr", argv[3], &L)) return 1;
    for (size_t i = 0; i < L.count; i++) {
        for (const char *p = L.line[i]; *p; p++) {
            size_t k = 0; while (k < nf && from[k] != *p) k++;
            if (k == nf) screen_putc(*p);
            else if (!del) screen_putc(to[k < nt ? k : nt - 1]);          /* a short <to> repeats its last character */
        }
        screen_putc('\n');
    }
    lines_free(&L);
    return 0;
}

static int cmd_join(int argc, const char **argv) {
    if (argc != 3) return cu_usage("join <separator> <file>     (puts all lines on one line)");
    lines_t L; if (!lines_load("join", argv[2], &L)) return 1;
    for (size_t i = 0; i < L.count; i++) { if (i) screen_puts(argv[1]); screen_puts(L.line[i]); }
    screen_putc('\n');
    lines_free(&L);
    return 0;
}
static int cmd_split(int argc, const char **argv) {
    if (argc < 3 || strlen(argv[1]) != 1) return cu_usage("split <char> <text...>     (one piece per line)");
    char d = argv[1][0];
    for (int i = 2; i < argc; i++) {
        if (i > 2) { if (d == ' ') screen_putc('\n'); else { screen_putc(' '); } }
        for (const char *p = argv[i]; *p; p++) screen_putc(*p == d ? '\n' : *p);
    }
    screen_putc('\n');
    return 0;
}
static int cmd_paste(int argc, const char **argv) {
    if (argc != 3) return cu_usage("paste <file1> <file2>     (lines side by side, separated by a tab)");
    lines_t A, B;
    if (!lines_load("paste", argv[1], &A)) return 1;
    if (!lines_load("paste", argv[2], &B)) { lines_free(&A); return 1; }
    size_t n = A.count > B.count ? A.count : B.count;
    for (size_t i = 0; i < n; i++) { if (i < A.count) screen_puts(A.line[i]); screen_putc('\t'); if (i < B.count) screen_puts(B.line[i]); screen_putc('\n'); }
    lines_free(&A); lines_free(&B);
    return 0;
}

/* ---------- numbers ---------- */
/* Numbers from a file (or a pipe: -f -): whitespace and commas separate them. */
static bool read_numbers_file(const char *cmd, const char *path, int64_t *v, int *n) {
    char *b; size_t len; *n = 0;
    if (!cu_load_file(cmd, path, &b, &len)) return false;
    size_t i = 0;
    while (i < len) {
        while (i < len && (b[i] == ' ' || b[i] == '\n' || b[i] == '\t' || b[i] == '\r' || b[i] == ',')) i++;
        if (i >= len) break;
        char tok[40]; size_t k = 0;
        while (i < len && !(b[i] == ' ' || b[i] == '\n' || b[i] == '\t' || b[i] == '\r' || b[i] == ',')) { if (k + 1 < sizeof tok) tok[k] = b[i]; k++; i++; }
        if (k >= sizeof tok) { cu_free_file(b); cu_error(cmd, "a value in the input is too long to be a number."); return false; }
        tok[k] = '\0';
        if (*n >= 64) { cu_free_file(b); cu_error(cmd, "at most 64 numbers."); return false; }
        if (calc_eval(tok, &v[*n]) != CALC_OK) { cu_free_file(b); cu_error(cmd, "the input contains something that is not a number:"); screen_puts("       "); screen_puts(tok); screen_putc('\n'); return false; }
        (*n)++;
    }
    cu_free_file(b);
    if (*n == 0) { cu_error(cmd, "no numbers in the input."); return false; }
    return true;
}
static bool read_numbers(const char *cmd, int argc, const char **argv, int64_t *v, int *n) {
    if (argc < 2) return false;
    if (argc == 3 && !strcmp(argv[1], "-f")) return read_numbers_file(cmd, argv[2], v, n);
    if (argc - 1 > 16) { cu_error(cmd, "at most 16 numbers."); return false; }
    for (int i = 1; i < argc; i++) {
        if (calc_eval(argv[i], &v[i - 1]) != CALC_OK) { cu_error(cmd, "not a number (integers and simple expressions only):"); screen_puts("       "); screen_puts(argv[i]); screen_putc('\n'); return false; }
    }
    *n = argc - 1;
    return true;
}
static int cmd_sum(int argc, const char **argv) {
    int64_t v[64], t = 0; int n;
    if (argc < 2) return cu_usage("sum <numbers...>   |   sum -f <file>");
    if (!read_numbers("sum", argc, argv, v, &n)) return 1;
    for (int i = 0; i < n; i++) if (math_add_ovf(t, v[i], &t)) return cu_error("sum", "the total does not fit in 64 bits.");
    cu_put_i64(t); screen_putc('\n');
    return 0;
}
static int cmd_min(int argc, const char **argv) {
    int64_t v[64]; int n;
    if (argc < 2) return cu_usage("min <numbers...>   |   min -f <file>");
    if (!read_numbers("min", argc, argv, v, &n)) return 1;
    int64_t m = v[0]; for (int i = 1; i < n; i++) if (v[i] < m) m = v[i];
    cu_put_i64(m); screen_putc('\n');
    return 0;
}
static int cmd_max(int argc, const char **argv) {
    int64_t v[64]; int n;
    if (argc < 2) return cu_usage("max <numbers...>   |   max -f <file>");
    if (!read_numbers("max", argc, argv, v, &n)) return 1;
    int64_t m = v[0]; for (int i = 1; i < n; i++) if (v[i] > m) m = v[i];
    cu_put_i64(m); screen_putc('\n');
    return 0;
}
static int cmd_avg(int argc, const char **argv) {
    int64_t v[64], t = 0, q, r; int n;
    if (argc < 2) return cu_usage("avg <numbers...>   |   avg -f <file>     (shows two decimals, integer arithmetic)");
    if (!read_numbers("avg", argc, argv, v, &n)) return 1;
    for (int i = 0; i < n; i++) if (math_add_ovf(t, v[i], &t)) return cu_error("avg", "the total does not fit in 64 bits.");
    math_idivmod(t, n, &q, &r);
    int64_t frac, dummy; math_idivmod((r < 0 ? -r : r) * 100, n, &frac, &dummy);
    if (q == 0 && r < 0) screen_putc('-');
    cu_put_i64(q); screen_putc('.'); if (frac < 10) screen_putc('0'); cu_put_u64((uint64_t)frac); screen_putc('\n');
    return 0;
}

/* ---------- strings ---------- */
static int cmd_strlen(int argc, const char **argv) {
    if (argc < 2) return cu_usage("strlen <text...>");
    size_t n = 0; for (int i = 1; i < argc; i++) n += strlen(argv[i]) + (i > 1 ? 1 : 0);
    screen_put_dec((uint32_t)n); screen_putc('\n');
    return 0;
}
static int cmd_repeatstr(int argc, const char **argv) {
    uint32_t n;
    if (argc != 3) return cu_usage("repeatstr <text> <count 0-200>");
    if (!cu_parse_count(argv[2], &n, 200)) return cu_error("repeatstr", "count must be 0 to 200.");
    if (strlen(argv[1]) * n > 2000) return cu_error("repeatstr", "result would be longer than 2000 characters.");
    for (uint32_t i = 0; i < n; i++) screen_puts(argv[1]);
    screen_putc('\n');
    return 0;
}
static bool at(const char *t, const char *p) { while (*p) { if (*t++ != *p++) return false; } return true; }   /* does t start with p? */

static int cmd_replace(int argc, const char **argv) {
    if (argc != 4) return cu_usage("replace <text> <old> <new>     (every occurrence)");
    const char *t = argv[1], *o = argv[2], *w = argv[3]; size_t ol = strlen(o);
    if (ol == 0) return cu_error("replace", "<old> must not be empty.");
    for (size_t i = 0; t[i]; ) {
        if (at(t + i, o)) { screen_puts(w); i += ol; } else screen_putc(t[i++]);
    }
    screen_putc('\n');
    return 0;
}

static int yes_no(bool v) { screen_puts(v ? "true\n" : "false\n"); return v ? 0 : 1; }
static int cmd_contains(int argc, const char **argv) {
    if (argc != 3) return cu_usage("contains <text> <part>     (prints true/false; exit status 0 = true)");
    for (const char *p = argv[1]; ; p++) { if (at(p, argv[2])) return yes_no(true); if (!*p) break; }
    return yes_no(false);
}
static int cmd_startswith(int argc, const char **argv) {
    if (argc != 3) return cu_usage("startswith <text> <prefix>");
    return yes_no(at(argv[1], argv[2]));
}
static int cmd_endswith(int argc, const char **argv) {
    if (argc != 3) return cu_usage("endswith <text> <suffix>");
    size_t tl = strlen(argv[1]), sl = strlen(argv[2]);
    return yes_no(sl <= tl && at(argv[1] + (tl - sl), argv[2]));
}

static int cmd_ascii(int argc, const char **argv) {
    if (argc == 2) {
        if (strlen(argv[1]) != 1) return cu_usage("ascii [character]");
        unsigned c = (unsigned char)argv[1][0];
        screen_puts("'"); screen_putc((char)c); screen_puts("' = decimal "); screen_put_dec(c); screen_puts(", hex "); screen_put_hex(c); screen_putc('\n');
        return 0;
    }
    if (argc != 1) return cu_usage("ascii [character]");
    screen_puts("Printable ASCII (decimal hex char)\n");
    for (unsigned row = 0; row < 24; row++) {
        for (unsigned col = 0; col < 4; col++) {
            unsigned c = 32 + col * 24 + row;
            if (c > 126) continue;
            cu_num_pad(c, 4);
            screen_puts("  ");
            static const char hx[] = "0123456789abcdef"; screen_putc(hx[c >> 4]); screen_putc(hx[c & 15]);
            screen_puts("  "); screen_putc((char)c); screen_puts("      ");
        }
        screen_putc('\n');
    }
    return 0;
}

/* ---------- wordfreq ---------- */
#define WF_MAX 256
typedef struct { char w[24]; uint32_t n; } wf_t;
static int cmd_wordfreq(int argc, const char **argv) {
    if (argc != 2) return cu_usage("wordfreq <file>     (case-insensitive; the 20 most frequent words)");
    char *b; size_t len;
    if (!cu_load_file("wordfreq", argv[1], &b, &len)) return 1;
    wf_t *t = sys_memory_alloc(WF_MAX * sizeof(wf_t));
    if (!t) { cu_free_file(b); return cu_error("wordfreq", "out of memory"); }
    size_t used = 0; uint32_t total = 0, dropped = 0;
    for (size_t i = 0; i < len; ) {
        while (i < len && !((b[i] >= 'a' && b[i] <= 'z') || (b[i] >= 'A' && b[i] <= 'Z') || (b[i] >= '0' && b[i] <= '9') || b[i] == '\'')) i++;
        if (i >= len) break;
        char w[24]; size_t k = 0;
        while (i < len && ((b[i] >= 'a' && b[i] <= 'z') || (b[i] >= 'A' && b[i] <= 'Z') || (b[i] >= '0' && b[i] <= '9') || b[i] == '\'')) { if (k < 23) w[k++] = lo(b[i]); i++; }
        w[k] = '\0'; total++;
        size_t j = 0; while (j < used && strcmp(t[j].w, w)) j++;
        if (j < used) t[j].n++;
        else if (used < WF_MAX) { memcpy(t[used].w, w, k + 1); t[used].n = 1; used++; }
        else dropped++;
    }
    for (size_t i = 1; i < used; i++) {                       /* insertion sort: count descending, then word ascending */
        wf_t x = t[i]; size_t j = i;
        while (j > 0 && (t[j - 1].n < x.n || (t[j - 1].n == x.n && strcmp(t[j - 1].w, x.w) > 0))) { t[j] = t[j - 1]; j--; }
        t[j] = x;
    }
    for (size_t i = 0; i < used && i < 20; i++) { cu_num_pad(t[i].n, 6); screen_putc(' '); put_line(t[i].w); }
    screen_put_dec(total); screen_puts(" words, "); screen_put_dec((uint32_t)used); screen_puts(" different");
    if (dropped) { screen_puts(" ("); screen_put_dec(dropped); screen_puts(" words not counted: more than 256 different words)"); }
    screen_putc('\n');
    sys_memory_free(t); cu_free_file(b);
    return 0;
}

static const struct { const char *name; cmd_handler_t fn; } bindings[] = {
    { "sort", cmd_sort }, { "uniq", cmd_uniq }, { "cut", cmd_cut }, { "tr", cmd_tr }, { "rev", cmd_rev }, { "nl", cmd_nl },
    { "tac", cmd_tac }, { "fold", cmd_fold }, { "upper", cmd_upper }, { "lower", cmd_lower }, { "capitalize", cmd_capitalize },
    { "sum", cmd_sum }, { "min", cmd_min }, { "max", cmd_max }, { "avg", cmd_avg }, { "join", cmd_join }, { "split", cmd_split },
    { "paste", cmd_paste }, { "strlen", cmd_strlen }, { "repeatstr", cmd_repeatstr }, { "trim", cmd_trim }, { "replace", cmd_replace },
    { "contains", cmd_contains }, { "startswith", cmd_startswith }, { "endswith", cmd_endswith }, { "ascii", cmd_ascii }, { "wordfreq", cmd_wordfreq },
};

int textops_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
