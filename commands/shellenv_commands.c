/* Shell-environment commands (Phase 11): exit alias unalias set unset env export source repeat test prompt banner.
 * These work on the shell's own state (lib/shellvars.c); files and tests go through system calls. */
#include "include/builtins.h"
#include "include/shell.h"
#include "include/cmdutil.h"
#include "include/screen.h"
#include "include/syscall.h"
#include "include/shellvars.h"
#include "include/mathlib.h"
#include "include/version.h"
#include "include/numparse.h"
#include "include/string.h"

static shell_exec_fn executor;
void misc_set_executor(shell_exec_fn fn);
void shellenv_set_executor(shell_exec_fn fn) { executor = fn; misc_set_executor(fn); }

static int sv_error(const char *cmd, const char *name, int rc) {
    cu_color(VGA_LIGHT_RED); screen_puts("Error: "); cu_color(VGA_LIGHT_GREY); screen_puts(cmd); screen_puts(": ");
    switch (rc) {
        case SV_ERR_NAME:      screen_puts("'"); screen_puts(name ? name : ""); screen_puts("' is not a valid name (letters, digits, _; up to 15; not starting with a digit).\n"); break;
        case SV_ERR_VALUE:     screen_puts("value too long (limit 63 characters, and aliases cannot be empty).\n"); break;
        case SV_ERR_FULL:      screen_puts("table full.\n"); break;
        case SV_ERR_NOT_FOUND: screen_puts("'"); screen_puts(name ? name : ""); screen_puts("' is not set.\n"); break;
        default:               screen_puts("failed.\n"); break;
    }
    return 1;
}

/* Joins argv[from..] with spaces. */
static bool join_args(int argc, const char **argv, int from, char *out, size_t cap) {
    size_t o = 0;
    for (int i = from; i < argc; i++) {
        size_t l = strlen(argv[i]);
        if (o + l + 2 > cap) return false;
        if (i > from) out[o++] = ' ';
        memcpy(out + o, argv[i], l); o += l;
    }
    out[o] = '\0';
    return true;
}

/* "NAME=value" or "NAME value ..." */
static int split_assignment(int argc, const char **argv, char *name, size_t ncap, char *value, size_t vcap, bool *has_value) {
    const char *eq = strchr(argv[1], '=');
    *has_value = true;
    if (eq) {
        size_t nl = (size_t)(eq - argv[1]);
        if (nl >= ncap) return SV_ERR_NAME;
        memcpy(name, argv[1], nl); name[nl] = '\0';
        char first[VAR_VALUE_MAX + 2]; size_t fl = strlen(eq + 1);
        if (fl >= sizeof first) return SV_ERR_VALUE;
        memcpy(first, eq + 1, fl + 1);
        if (argc == 2) { if (fl >= vcap) return SV_ERR_VALUE; memcpy(value, first, fl + 1); return SV_OK; }
        const char *rest[16]; int n = 0; rest[n++] = first;
        for (int i = 2; i < argc && n < 16; i++) rest[n++] = argv[i];
        return join_args(n, rest, 0, value, vcap) ? SV_OK : SV_ERR_VALUE;
    }
    size_t nl = strlen(argv[1]);
    if (nl >= ncap) return SV_ERR_NAME;
    memcpy(name, argv[1], nl + 1);
    if (argc == 2) { *has_value = false; value[0] = '\0'; return SV_OK; }
    return join_args(argc, argv, 2, value, vcap) ? SV_OK : SV_ERR_VALUE;
}

static int cmd_set(int argc, const char **argv) {
    if (argc == 1) {
        const char *n, *v; bool ex;
        for (size_t i = 0; var_at(i, &n, &v, &ex); i++) { screen_puts(n); screen_putc('='); screen_puts(v); screen_putc('\n'); }
        if (var_count() == 0) screen_puts("(no variables set; try: set GREETING hello, then: echo $GREETING)\n");
        return 0;
    }
    char name[VAR_NAME_MAX + 4], value[VAR_VALUE_MAX + 2]; bool has;
    int rc = split_assignment(argc, argv, name, sizeof name, value, sizeof value, &has);
    if (rc == SV_OK && !has) value[0] = '\0';
    if (rc == SV_OK) rc = var_set(name, value, false);
    return rc == SV_OK ? 0 : sv_error("set", name, rc);
}

static int cmd_unset(int argc, const char **argv) {
    if (argc < 2) return cu_usage("unset <name...>");
    int status = 0;
    for (int i = 1; i < argc; i++) { int rc = var_unset(argv[i]); if (rc != SV_OK) status = sv_error("unset", argv[i], rc); }
    return status;
}

static int cmd_export(int argc, const char **argv) {
    if (argc < 2) return cu_usage("export NAME[=value]");
    char name[VAR_NAME_MAX + 4], value[VAR_VALUE_MAX + 2]; bool has;
    int rc = split_assignment(argc, argv, name, sizeof name, value, sizeof value, &has);
    if (rc == SV_OK && has) rc = var_set(name, value, true);
    else if (rc == SV_OK) rc = var_export(name);
    return rc == SV_OK ? 0 : sv_error("export", name, rc);
}

static int cmd_env(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("env");
    const char *n, *v; bool ex; uint32_t shown = 0;
    for (size_t i = 0; var_at(i, &n, &v, &ex); i++) if (ex) { screen_puts(n); screen_putc('='); screen_puts(v); screen_putc('\n'); shown++; }
    if (!shown) screen_puts("(nothing exported; use: export NAME=value)\n");
    return 0;
}

static int cmd_alias(int argc, const char **argv) {
    if (argc == 1) {
        const char *n, *v;
        for (size_t i = 0; alias_at(i, &n, &v); i++) { screen_puts("alias "); screen_puts(n); screen_puts("='"); screen_puts(v); screen_puts("'\n"); }
        if (alias_count() == 0) screen_puts("(no aliases; try: alias ll=\"ls -l\")\n");
        return 0;
    }
    char name[VAR_NAME_MAX + 4], value[VAR_VALUE_MAX + 2]; bool has;
    int rc = split_assignment(argc, argv, name, sizeof name, value, sizeof value, &has);
    if (rc != SV_OK) return sv_error("alias", name, rc);
    if (!has) {                                                  /* alias NAME: show it */
        const char *v = alias_get(name);
        if (!v) return sv_error("alias", name, SV_ERR_NOT_FOUND);
        screen_puts("alias "); screen_puts(name); screen_puts("='"); screen_puts(v); screen_puts("'\n");
        return 0;
    }
    if (command_find(name) && !strcmp(name, "alias")) return cu_error("alias", "cannot alias 'alias' itself.");
    rc = alias_set(name, value);
    return rc == SV_OK ? 0 : sv_error("alias", name, rc);
}

static int cmd_unalias(int argc, const char **argv) {
    if (argc < 2) return cu_usage("unalias <name...>");
    int status = 0;
    for (int i = 1; i < argc; i++) { int rc = alias_remove(argv[i]); if (rc != SV_OK) status = sv_error("unalias", argv[i], rc); }
    return status;
}

static int cmd_prompt(int argc, const char **argv) {
    if (argc == 1) { const char *p = var_get("PS1"); screen_puts(p ? p : "(default: user@MiniOS> )"); screen_putc('\n'); return 0; }
    if (argc == 2 && !strcmp(argv[1], "default")) { var_unset("PS1"); screen_puts("Prompt restored.\n"); return 0; }
    char text[VAR_VALUE_MAX + 2];
    if (!join_args(argc, argv, 1, text, sizeof text)) return cu_error("prompt", "text too long (limit 63 characters).");
    int rc = var_set("PS1", text, false);
    return rc == SV_OK ? 0 : sv_error("prompt", "PS1", rc);
}

static int cmd_exit(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("exit     (ends the session, same as logout)");
    int rc = sys_logout();
    if (rc != 0) return cu_error("exit", sys_strerror(rc));
    screen_puts("Session ended.\n");
    return 0;
}

static int cmd_banner(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("banner");
    cu_color(VGA_LIGHT_CYAN);
    screen_puts("  __  __ _       _  ___  ____\n |  \\/  (_)_ __ (_)/ _ \\/ ___|\n | |\\/| | | '_ \\| | | | \\___ \\\n | |  | | | | | | | |_| |___) |\n |_|  |_|_|_| |_|_|\\___/|____/\n");
    cu_color(VGA_LIGHT_GREY);
    screen_puts("  Educational Operating System v" MINIOS_VERSION " (" MINIOS_PHASE ", i386)\n");
    return 0;
}

/* ---------- test ---------- */
static bool num_arg(const char *s, int64_t *v) { return calc_eval(s, v) == CALC_OK; }

/* Evaluates the tokens; returns 0 true, 1 false, 2 syntax error. */
static int eval_test(int n, const char **a) {
    if (n == 0) return 1;
    if (!strcmp(a[0], "!")) { int r = eval_test(n - 1, a + 1); return r == 2 ? 2 : !r; }
    if (n == 1) return a[0][0] ? 0 : 1;                                   /* a lone string is true if non-empty */
    if (n == 2) {
        const char *op = a[0], *arg = a[1];
        if (!strcmp(op, "-z")) return arg[0] == '\0' ? 0 : 1;
        if (!strcmp(op, "-n")) return arg[0] != '\0' ? 0 : 1;
        fs_stat_t st; int rc;
        if (!strcmp(op, "-e") || !strcmp(op, "-f") || !strcmp(op, "-d") || !strcmp(op, "-s")) {
            rc = sys_stat(arg, &st);
            if (rc != 0) return 1;
            if (!strcmp(op, "-f")) return st.type == FS_FILE ? 0 : 1;
            if (!strcmp(op, "-d")) return st.type == FS_DIR ? 0 : 1;
            if (!strcmp(op, "-s")) return st.size > 0 ? 0 : 1;
            return 0;
        }
        if (!strcmp(op, "-r")) { int fd = sys_open(arg, SYS_O_READ); if (fd < 0) return 1; sys_close(fd); return 0; }
        return 2;
    }
    if (n == 3) {
        const char *x = a[0], *op = a[1], *y = a[2];
        if (!strcmp(op, "=")) return !strcmp(x, y) ? 0 : 1;
        if (!strcmp(op, "!=")) return strcmp(x, y) ? 0 : 1;
        static const char *const nops[] = { "-eq", "-ne", "-lt", "-le", "-gt", "-ge" };
        for (int k = 0; k < 6; k++) if (!strcmp(op, nops[k])) {
            int64_t p, q;
            if (!num_arg(x, &p) || !num_arg(y, &q)) return 2;
            bool r = k == 0 ? p == q : k == 1 ? p != q : k == 2 ? p < q : k == 3 ? p <= q : k == 4 ? p > q : p >= q;
            return r ? 0 : 1;
        }
    }
    return 2;
}

static int cmd_test(int argc, const char **argv) {
    int r = eval_test(argc - 1, argv + 1);
    if (r == 2) { cu_error("test", "cannot understand that condition."); screen_puts("       forms: -e|-f|-d|-s|-r PATH, -z|-n STR, A = B, A != B, N -eq|-ne|-lt|-le|-gt|-ge M, ! COND\n"); return 2; }
    return r;                                                   /* 0 = true, 1 = false; nothing is printed */
}

/* ---------- source and repeat ---------- */
static int source_depth;

static int cmd_source(int argc, const char **argv) {
    if (argc != 2) return cu_usage("source <file>     (runs each line; an if/for/while may span lines; blank lines and # comments are skipped)");
    if (!executor) return cu_error("source", "no shell is available to run the lines.");
    if (source_depth >= 4) return cu_error("source", "scripts nested too deeply (limit 4).");
    char *text; size_t len;
    if (!cu_load_file("source", argv[1], &text, &len)) return 1;
    int last = 0; uint32_t lineno = 0; source_depth++;
    char stmt[512]; size_t slen = 0; uint32_t first_line = 0;          /* a statement can span several lines: an if/for/while is read until its fi/done */
    for (size_t pos = 0; pos < len; ) {
        size_t e = pos; while (e < len && text[e] != '\n') e++;
        char line[128]; size_t l = e - pos; lineno++;
        if (l >= sizeof line) { cu_error("source", "a line is longer than 127 characters; stopped."); last = 1; slen = 0; break; }
        memcpy(line, text + pos, l); line[l] = '\0';
        pos = e + 1;
        size_t k = 0; while (line[k] == ' ' || line[k] == '\t') k++;
        if (line[k] == '\0' || line[k] == '#' || line[k] == '\r') continue;
        size_t ll = strlen(line + k);
        while (ll && (line[k + ll - 1] == '\r' || line[k + ll - 1] == ' ' || line[k + ll - 1] == '\t')) line[k + --ll] = '\0';
        if (slen == 0) first_line = lineno;
        size_t need = ll + (slen ? 3 : 0);
        if (slen + need >= sizeof stmt) { cu_error("source", "a block is longer than 511 characters; stopped."); last = 1; slen = 0; break; }
        if (slen) { stmt[slen++] = ' '; stmt[slen++] = ';'; stmt[slen++] = ' '; }
        memcpy(stmt + slen, line + k, ll); slen += ll; stmt[slen] = '\0';
        if (command_list_incomplete(stmt)) continue;                     /* inside an if/for/while: read the next line too */
        last = executor(stmt);
        slen = 0;
    }
    if (slen) {                                                          /* the file ended inside a block */
        screen_puts("Error: source: the file ends inside an unfinished if/for/while that began on line "); screen_put_dec(first_line); screen_puts(" (missing fi or done); nothing from it was run.\n");
        last = 2;
    }
    source_depth--;
    cu_free_file(text);
    return last;                                                 /* status of the last command that ran */
}

static int cmd_repeat(int argc, const char **argv) {
    if (argc < 3) return cu_usage("repeat <count 1-100> <command> [args...]");
    uint32_t n;
    if (!cu_parse_count(argv[1], &n, 100) || n == 0) return cu_error("repeat", "count must be between 1 and 100.");
    if (!strcmp(argv[2], "repeat") || !strcmp(argv[2], "source")) return cu_error("repeat", "cannot repeat 'repeat' or 'source'.");
    const command_t *c = command_find(argv[2]);
    if (!c) { cu_color(VGA_LIGHT_RED); screen_puts("Error: "); cu_color(VGA_LIGHT_GREY); screen_puts("repeat: unknown command '"); screen_puts(argv[2]); screen_puts("'.\n"); return 127; }
    int status = 0;
    for (uint32_t i = 0; i < n; i++) status = c->handler ? c->handler(argc - 2, argv + 2) : command_stub(c, argc - 2, argv + 2);
    return status;
}

static const struct { const char *name; cmd_handler_t fn; } bindings[] = {
    { "exit", cmd_exit }, { "alias", cmd_alias }, { "unalias", cmd_unalias }, { "set", cmd_set }, { "unset", cmd_unset },
    { "env", cmd_env }, { "export", cmd_export }, { "source", cmd_source }, { "repeat", cmd_repeat }, { "test", cmd_test },
    { "prompt", cmd_prompt }, { "banner", cmd_banner },
};

int shellenv_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
