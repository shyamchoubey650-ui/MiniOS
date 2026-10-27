/* Kernel-log commands (Phase 12): logs dmesg loglevel logclear logsave logtail logfind logstats trace strace schedtrace.
 *
 * All of them read the REAL kernel log (include/logger.h) through the system calls SYS_LOG_GET / SYS_LOG_CTL.
 * Changing the log (loglevel, logclear, trace, schedtrace) and saving it (logsave) need root; reading is open to everybody,
 * except that entries from the private sources "sec" and "shell" (logins, commands run) are shown to root only. */
#include "include/builtins.h"
#include "include/cmdutil.h"
#include "include/screen.h"
#include "include/syscall.h"
#include "include/keyboard.h"
#include "include/textutil.h"
#include "include/numparse.h"
#include "include/string.h"

/* ---------- helpers ---------- */
static bool is_root(void) { cred_info_t c; return sys_getcred(&c) == 0 && c.euid == 0; }
static int denied(const char *cmd) { return cu_error(cmd, "permission denied: only root can do this (try sudo)"); }

/* Copies the entries the caller may see into one heap block. *n may be 0 (then *out is NULL). */
static bool load(const char *cmd, int which, log_entry_t **out, int *n) {
    int count = sys_log_get(which, 0, NULL);
    *out = NULL; *n = 0;
    if (count < 0) { cu_error(cmd, "cannot read the kernel log"); return false; }
    if (count == 0) return true;
    log_entry_t *b = sys_memory_alloc(sizeof(log_entry_t) * (size_t)count);
    if (!b) { cu_error(cmd, "out of memory"); return false; }
    int got = 0;
    for (int i = 0; i < count; i++) if (sys_log_get(which, (uint32_t)i, &b[got]) == 0) got++;      /* private boot entries are skipped, not errors */
    *out = b; *n = got;
    return true;
}
static void release(log_entry_t *b) { if (b) sys_memory_free(b); }

static void print_entry(const log_entry_t *e) {
    char line[112];
    klog_format(e, line, sizeof line);
    cu_color(e->level == LOG_ERROR ? VGA_LIGHT_RED : e->level == LOG_WARN ? VGA_YELLOW : e->level == LOG_DEBUG ? VGA_DARK_GREY : VGA_LIGHT_GREY);
    screen_puts(line); cu_color(VGA_LIGHT_GREY); screen_putc('\n');
}
static bool src_is(const log_entry_t *e, const char *s) { return !strcmp(e->src, s); }

/* Parses "-l level" at argv[i]; returns false (and prints) on a bad level. */
static bool parse_level(const char *cmd, const char *s, int *lv) {
    *lv = klog_level_from_name(s);
    if (*lv < 0) { cu_error(cmd, "level must be debug, info, warn or error"); return false; }
    return true;
}

/* ---------- logs ---------- */
static int cmd_logs(int argc, const char **argv) {
    uint32_t last = 20; bool all = false; int min = LOG_DEBUG; const char *src = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) all = true;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) { if (!cu_parse_count(argv[++i], &last, LOG_CAPACITY) || last == 0) return cu_error("logs", "-n needs a number from 1 to 256"); }
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) { if (!parse_level("logs", argv[++i], &min)) return 1; }
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) src = argv[++i];
        else return cu_usage("logs [-a | -n N] [-l debug|info|warn|error] [-s source]      (default: the last 20 entries)");
    }
    log_entry_t *b; int n;
    if (!load("logs", SYS_LOG_RING, &b, &n)) return 1;
    int *idx = n ? sys_memory_alloc(sizeof(int) * (size_t)n) : NULL;
    int m = 0;
    if (n && !idx) { release(b); return cu_error("logs", "out of memory"); }
    for (int i = 0; i < n; i++) if (b[i].level >= min && (!src || src_is(&b[i], src))) idx[m++] = i;
    int from = (all || (uint32_t)m <= last) ? 0 : m - (int)last;
    for (int k = from; k < m; k++) print_entry(&b[idx[k]]);
    if (m == 0) screen_puts("(no matching log entries)\n");
    else if (from > 0) { screen_puts("("); screen_put_dec((uint32_t)from); screen_puts(" older entries not shown: use -a for all, or -n N)\n"); }
    if (idx) sys_memory_free(idx);
    release(b);
    return 0;
}

/* ---------- dmesg ---------- */
static int cmd_dmesg(int argc, const char **argv) {
    int min = LOG_DEBUG;
    if (argc == 3 && !strcmp(argv[1], "-l")) { if (!parse_level("dmesg", argv[2], &min)) return 1; }
    else if (argc != 1) return cu_usage("dmesg [-l level]      (the messages the kernel printed while booting)");
    log_entry_t *b; int n;
    if (!load("dmesg", SYS_LOG_BOOT, &b, &n)) return 1;
    for (int i = 0; i < n; i++) if (b[i].level >= min) print_entry(&b[i]);
    if (n == 0) screen_puts("(no boot messages kept)\n");
    release(b);
    return 0;
}

/* ---------- loglevel ---------- */
static const char *level_meaning(int l) {
    return l == LOG_DEBUG ? "everything is stored (very chatty)" : l == LOG_INFO ? "normal events and everything more serious" : l == LOG_WARN ? "only warnings and errors" : "only errors";
}
static int cmd_loglevel(int argc, const char **argv) {
    log_stats_t s;
    if (argc > 2) return cu_usage("loglevel [debug|info|warn|error]      (root can change it; entries below the level are not stored)");
    if (argc == 2) {
        int lv; if (!parse_level("loglevel", argv[1], &lv)) return 1;
        int rc = sys_log_ctl(SYS_LOGCTL_SET_LEVEL, (uint32_t)lv, NULL);
        if (rc == SYS_EACCES) return denied("loglevel");
        if (rc < 0) return cu_error("loglevel", sys_strerror(rc));
    }
    sys_log_ctl(SYS_LOGCTL_STATS, 0, &s);
    screen_puts("minimum level: "); screen_puts(klog_level_name(s.min_level)); screen_puts(" - "); screen_puts(level_meaning(s.min_level)); screen_putc('\n');
    if (argc == 1) { screen_put_dec(s.dropped_below_level); screen_puts(" entries have been refused because of the level\n"); }
    return 0;
}

/* ---------- logclear ---------- */
static int cmd_logclear(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("logclear      (root: empties the log; the clearing itself is logged)");
    int rc = sys_log_ctl(SYS_LOGCTL_CLEAR, 0, NULL);
    if (rc == SYS_EACCES) return denied("logclear");
    if (rc < 0) return cu_error("logclear", sys_strerror(rc));
    screen_puts("Log cleared. The boot messages are still available with 'dmesg'.\n");
    return 0;
}

/* ---------- logsave ---------- */
static int cmd_logsave(int argc, const char **argv) {
    const char *path = "/var/log/system.log";
    if (argc > 2) return cu_usage("logsave [file]      (root; default /var/log/system.log; replaces the file)");
    if (argc == 2) path = argv[1];
    if (!is_root()) return denied("logsave");
    log_entry_t *b; int n;
    if (!load("logsave", SYS_LOG_RING, &b, &n)) return 1;
    size_t cap = (size_t)n * 112 + 1;
    char *text = sys_memory_alloc(cap);
    if (!text) { release(b); return cu_error("logsave", "out of memory"); }
    size_t w = 0;
    for (int i = 0; i < n; i++) { w += klog_format(&b[i], text + w, cap - w); text[w++] = '\n'; }
    release(b);
    if (!strcmp(path, "/var/log/system.log")) sys_mkdir("/var/log", SYS_MKDIR_PARENTS);       /* the default directory is created on demand */
    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREATE | SYS_O_TRUNC);
    if (fd < 0) { sys_memory_free(text); return cu_fs_error("logsave", path, fd, CU_FILE); }
    int rc = w ? sys_write(fd, text, w) : 0;
    sys_close(fd); sys_memory_free(text);
    if (rc < 0) return cu_fs_error("logsave", path, rc, CU_FILE);
    screen_puts("Saved "); screen_put_dec((uint32_t)n); screen_puts(" entries ("); screen_put_dec((uint32_t)w); screen_puts(" bytes) to "); screen_puts(path); screen_putc('\n');
    return 0;
}

/* ---------- logtail ---------- */
static bool ctrl_c_pending(void) {
    bool hit = false;
    while (keyboard_has_key()) if (keyboard_getkey() == 3) hit = true;
    return hit;
}
static int cmd_logtail(int argc, const char **argv) {
    uint32_t last = 10; bool follow = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) follow = true;
        else if (cu_parse_count(argv[i], &last, 200) && last > 0) { }
        else return cu_usage("logtail [N] [-f]      (last N entries, default 10; -f keeps showing new ones until Ctrl+C)");
    }
    log_entry_t *b; int n;
    if (!load("logtail", SYS_LOG_RING, &b, &n)) return 1;
    uint32_t seen_seq = 0;
    for (int i = n - (int)(last < (uint32_t)n ? last : (uint32_t)n); i < n; i++) { print_entry(&b[i]); seen_seq = b[i].seq; }
    if (n == 0) screen_puts("(the log is empty)\n");
    if (n) seen_seq = b[n - 1].seq;
    release(b);
    if (!follow) return 0;
    screen_puts("-- following the log, Ctrl+C to stop --\n");
    for (;;) {
        if (ctrl_c_pending()) break;
        if (!load("logtail", SYS_LOG_RING, &b, &n)) return 1;
        for (int i = 0; i < n; i++) if (b[i].seq > seen_seq) { print_entry(&b[i]); seen_seq = b[i].seq; }
        release(b);
        if (ctrl_c_pending()) break;
        cu_sleep_ms(200);
    }
    screen_puts("\n");
    return 0;
}

/* ---------- logfind ---------- */
static int cmd_logfind(int argc, const char **argv) {
    bool exact_case = false; int i = 1;
    if (i < argc && !strcmp(argv[i], "-c")) { exact_case = true; i++; }
    if (argc - i != 1) return cu_usage("logfind [-c] <text>      (entries whose message or source contains the text; -c = match upper/lower case)");
    log_entry_t *b; int n;
    if (!load("logfind", SYS_LOG_RING, &b, &n)) return 1;
    uint32_t hits = 0;
    for (int k = 0; k < n; k++)
        if (text_contains(b[k].msg, strlen(b[k].msg), argv[i], !exact_case) || text_contains(b[k].src, strlen(b[k].src), argv[i], !exact_case)) { print_entry(&b[k]); hits++; }
    screen_put_dec(hits); screen_puts(hits == 1 ? " entry matches\n" : " entries match\n");
    release(b);
    return hits ? 0 : 1;
}

/* ---------- logstats ---------- */
static int cmd_logstats(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("logstats");
    log_stats_t s;
    if (sys_log_ctl(SYS_LOGCTL_STATS, 0, &s) < 0) return cu_error("logstats", "cannot read the log statistics");
    screen_puts("capacity          "); screen_put_dec(s.capacity); screen_puts(" entries (the oldest are overwritten)\n");
    screen_puts("stored now        "); screen_put_dec(s.stored); screen_putc('\n');
    screen_puts("written in total  "); screen_put_dec(s.total_written); screen_putc('\n');
    screen_puts("overwritten       "); screen_put_dec(s.overwritten); screen_putc('\n');
    screen_puts("below the level   "); screen_put_dec(s.dropped_below_level); screen_puts(" refused\n");
    screen_puts("minimum level     "); screen_puts(klog_level_name(s.min_level)); screen_putc('\n');
    screen_puts("\nlevel   stored\n");
    for (int l = LOG_LEVELS - 1; l >= 0; l--) { cu_pad(klog_level_name(l), 8); screen_put_dec(s.per_level[l]); screen_putc('\n'); }
    screen_puts("\nsyscall trace     "); screen_puts(s.trace_syscalls ? "ON" : "off"); screen_puts("  ("); screen_put_dec(s.trace_count); screen_puts(" calls traced)\n");
    screen_puts("scheduler trace   "); screen_puts(s.trace_sched ? "ON" : "off"); screen_puts("  ("); screen_put_dec(s.sched_count); screen_puts(" switches traced)\n");
    screen_puts("boot messages     "); screen_put_dec(s.boot_entries); screen_putc('\n');
    if (s.hidden_private) { screen_put_dec(s.hidden_private); screen_puts(" entries (logins, commands) are visible to root only\n"); }
    return 0;
}

/* ---------- trace / schedtrace / strace ---------- */
static int toggle(const char *cmd, int op, const char *what, int argc, const char **argv) {
    log_stats_t s;
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "on") && strcmp(argv[1], "off"))) {
        char u[96]; size_t n = strlen(cmd); memcpy(u, cmd, n); const char *rest = " [on|off]"; memcpy(u + n, rest, strlen(rest) + 1);
        return cu_usage(u);
    }
    if (argc == 2) {
        int rc = sys_log_ctl(op, strcmp(argv[1], "on") == 0 ? 1u : 0u, NULL);
        if (rc == SYS_EACCES) return denied(cmd);
        if (rc < 0) return cu_error(cmd, sys_strerror(rc));
    }
    sys_log_ctl(SYS_LOGCTL_STATS, 0, &s);
    bool on = op == SYS_LOGCTL_TRACE ? s.trace_syscalls : s.trace_sched;
    screen_puts(what); screen_puts(" is "); screen_puts(on ? "ON" : "off"); screen_putc('\n');
    return 0;
}
static int cmd_trace(int argc, const char **argv) {
    int rc = toggle("trace", SYS_LOGCTL_TRACE, "system-call trace", argc, argv);
    if (rc == 0 && argc == 2 && !strcmp(argv[1], "on")) screen_puts("Every system call is now logged (source 'trace'). See them with: logs -s trace   or   strace <pid>\nThe log only holds 256 entries, so turn the trace off when you are done.\n");
    return rc;
}
static int cmd_schedtrace(int argc, const char **argv) {
    int rc = toggle("schedtrace", SYS_LOGCTL_SCHED, "scheduler trace", argc, argv);
    if (rc != 0 || argc == 2) return rc;
    log_entry_t *b; int n;
    if (!load("schedtrace", SYS_LOG_RING, &b, &n)) return 1;
    int total = 0;
    for (int i = 0; i < n; i++) if (src_is(&b[i], "sched")) total++;
    if (total == 0) screen_puts("(no scheduling decisions logged yet: 'sudo schedtrace on', then run 'spawn demo 2')\n");
    else {
        int start = total > 12 ? total - 12 : 0, seen = 0;
        screen_puts("last scheduling decisions:\n");
        for (int i = 0; i < n; i++) if (src_is(&b[i], "sched") && seen++ >= start) print_entry(&b[i]);
    }
    release(b);
    return 0;
}
static int cmd_strace(int argc, const char **argv) {
    uint32_t pid, last = 20;
    if (argc < 2 || argc > 3 || !cu_parse_count(argv[1], &pid, 100000) || (argc == 3 && (!cu_parse_count(argv[2], &last, LOG_CAPACITY) || last == 0)))
        return cu_usage("strace <pid> [last N]      (the system calls this process made while 'trace on' was active; the log holds only the newest 256 entries)");
    log_entry_t *b; int n;
    if (!load("strace", SYS_LOG_RING, &b, &n)) return 1;
    int hits = 0;
    for (int i = 0; i < n; i++) if (src_is(&b[i], "trace") && b[i].pid == (int)pid) hits++;
    int skip = hits > (int)last ? hits - (int)last : 0, seen = 0;
    for (int i = 0; i < n; i++) if (src_is(&b[i], "trace") && b[i].pid == (int)pid && seen++ >= skip) print_entry(&b[i]);
    if (hits == 0) screen_puts("no traced system calls for that process (turn on the trace first: 'sudo trace on'; entries may also have been overwritten)\n");
    else if (skip) { screen_put_dec((uint32_t)skip); screen_puts(" earlier calls not shown\n"); }
    release(b);
    return hits ? 0 : 1;
}

static const struct { const char *name; cmd_handler_t fn; } bindings[] = {
    { "logs", cmd_logs }, { "dmesg", cmd_dmesg }, { "loglevel", cmd_loglevel }, { "logclear", cmd_logclear }, { "logsave", cmd_logsave },
    { "logtail", cmd_logtail }, { "logfind", cmd_logfind }, { "logstats", cmd_logstats }, { "trace", cmd_trace }, { "strace", cmd_strace },
    { "schedtrace", cmd_schedtrace },
};

int log_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
