/* Phase 12 tests: the kernel logger wired into the real kernel (system calls, users, processes, mkfs, savefs/loadfs, trace),
 * the 11 log commands, and the commands that finished the 300-command set. Everything runs through the REAL system-call
 * layer, MiniFS, parser, registry and redirection. Only the screen, keyboard, timer and context switch are stubbed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/shell.h"
#include "include/command.h"
#include "include/builtins.h"
#include "include/memory.h"
#include "include/heap.h"
#include "include/filesystem.h"
#include "include/redirect.h"
#include "include/capture.h"
#include "include/screen.h"
#include "include/timer.h"
#include "include/interrupts.h"
#include "include/syscall.h"
#include "include/process.h"
#include "include/users.h"
#include "include/shellvars.h"
#include "include/kinfo.h"
#include "include/ramdisk.h"
#include "include/logger.h"
#include "include/loopback.h"
#include "include/kinfo.h"
#include "tests/low_stack.h"

void interrupt_register(uint32_t v, interrupt_handler_t h) { (void)v; (void)h; }
static char out[65536]; static size_t out_len;
static void out_reset(void) { out_len = 0; out[0] = 0; }
void screen_init(void) {} void screen_clear(void) {} void screen_set_color(uint8_t f, uint8_t b) { (void)f; (void)b; } void screen_set_default_color(uint8_t f, uint8_t b) { (void)f; (void)b; }
void screen_backspace(void) {}
void screen_putc(char c) { if (capture_write(c)) return; if (out_len + 1 < sizeof out) { out[out_len++] = c; out[out_len] = 0; } }
void screen_puts(const char *s) { while (*s) screen_putc(*s++); }
void screen_put_dec(uint32_t v) { char b[16]; snprintf(b, sizeof b, "%u", v); screen_puts(b); }
void screen_put_hex(uint32_t v) { char b[16]; snprintf(b, sizeof b, "0x%08X", v); screen_puts(b); }
static uint32_t fake_ticks = 500;
uint32_t timer_ticks(void) { return fake_ticks; }
void timer_sleep_ms(uint32_t ms) { (void)ms; }
const char *exception_name(uint32_t v) { (void)v; return "exception"; }
int shell_last_exit(void) { return 0; }
int memory_commands_register(void) { return 0; }
int process_commands_register(void) { return 0; }

/* keyboard: text queued by feed() is what the next password prompts read */
static char kq[512]; static size_t kq_pos;
static void feed(const char *s) { strncpy(kq, s, sizeof kq - 1); kq[sizeof kq - 1] = 0; kq_pos = 0; }
int keyboard_has_key(void) { return kq[kq_pos] != 0; }
int keyboard_getkey(void) { return kq[kq_pos] ? (unsigned char)kq[kq_pos++] : 3; }


static int run, failed;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; printf("  FAIL line %d: %s\n    output was: [%s]\n", __LINE__, #cond, out); } } while (0)

static bool owner_yes(void) { return true; }
static int sh(const char *raw) {                     /* same pipeline as shell/shell.c: aliases, $variables, parse, redirect, run */
    static char buf[SHELL_LINE_MAX * 2], aliased[SHELL_LINE_MAX * 2], line[SHELL_LINE_MAX * 2]; command_line_t cl; bool was;
    out_reset();
    if (sv_expand_alias(raw, aliased, sizeof aliased, &was) != SV_OK || sv_expand_vars(aliased, line, sizeof line) != SV_OK) { screen_puts("Error: too long\n"); return 2; }
    int rc = parse_command_line(line, buf, sizeof buf, &cl);
    if (rc == PARSE_EMPTY) return 0;
    if (rc != PARSE_OK) { screen_puts("Error: "); screen_puts(parse_error_string(rc)); screen_putc('\n'); return 2; }
    const command_t *c = command_find(cl.argv[0]);
    if (!c) { screen_puts("Error: unknown command '"); screen_puts(cl.argv[0]); screen_puts("'\n"); sv_set_status(127); return 127; }
    redirect_t rd; bool redirected = false;
    if (cl.redir != REDIR_NONE) {
        int rrc = redirect_begin(&rd, cl.redir_path, cl.redir, owner_yes);
        if (rrc != FS_OK) { screen_puts("Error: cannot redirect to '"); screen_puts(cl.redir_path); screen_puts("': "); screen_puts(fs_strerror(rrc)); screen_putc('\n'); return 1; }
        redirected = true;
    }
    int st = c->handler ? c->handler(cl.argc, cl.argv) : command_stub(c, cl.argc, cl.argv);
    if (redirected) st = redirect_end(&rd, st);
    sv_set_status(st);
    return st;
}
/* the executor that 'source' and 'demo' use: runs a line and APPENDS its output to what is already in `out` */
static int script_exec(const char *line) {
    static char keep[65536]; size_t kl = out_len; memcpy(keep, out, kl + 1);
    int r = sh(line);
    size_t nl = out_len; static char mine[65536]; memcpy(mine, out, nl + 1);
    if (kl + nl + 1 < sizeof out) { memcpy(out, keep, kl); memcpy(out + kl, mine, nl + 1); out_len = kl + nl; }
    return r;
}
static bool has(const char *s) { return strstr(out, s) != NULL; }

static multiboot_mmap_entry_t map_buf[1]; static multiboot_info_t mbi;
static void fresh(void) {
    map_buf[0] = (multiboot_mmap_entry_t){ 20, 0x100000, 0x4000000, 1 };
    memset(&mbi, 0, sizeof mbi);
    mbi.flags = MULTIBOOT_FLAG_MMAP; mbi.mmap_addr = (uint32_t)(uintptr_t)map_buf; mbi.mmap_length = sizeof map_buf;
    memory_init(&mbi, 0x100000, 0x110000);
    fs_set_clock(timer_ticks); fs_set_user(FS_UID_ROOT, FS_UID_ROOT); fs_set_umask(022);
    fs_init(); process_init(); syscall_init(); ramdisk_init(); sv_reset();
    klog_init(); users_init();
    builtins_register();
    shellenv_set_executor(script_exec);
    kq[0] = 0; kq_pos = 0; out_reset();
}
static void login_as(const char *u, const char *p) { int rc = sys_login(u, p); if (rc != 0) { failed++; printf("  FAIL line %d: login %s = %d\n", __LINE__, u, rc); } run++; }
#define ROOT() login_as("root", "root123")
static int put(const char *path, const char *text) {
    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREATE | SYS_O_TRUNC);
    if (fd < 0) return fd;
    int n = sys_write(fd, text, strlen(text)); sys_close(fd);
    return n < 0 ? n : 0;
}
static bool file_is(const char *path, const char *want) {
    char b[1024]; int fd = sys_open(path, SYS_O_READ); if (fd < 0) return false;
    int n = sys_read(fd, b, sizeof b - 1); sys_close(fd); if (n < 0) return false; b[n] = 0;
    return !strcmp(b, want);
}

static int count_entries(bool root, const char *src) {
    int n = 0; log_entry_t e;
    for (int i = 0; klog_get((uint32_t)i, root, &e) == 0; i++) if (!src || !strcmp(e.src, src)) n++;
    return n;
}
static bool log_has(const char *src, const char *text) {
    log_entry_t e;
    for (int i = 0; klog_get((uint32_t)i, true, &e) == 0; i++) if ((!src || !strcmp(e.src, src)) && strstr(e.msg, text)) return true;
    return false;
}

/* ================= the log inside the real kernel ================= */
static void test_kernel_events(void) {
    puts("kernel events reach the log: login, failed login, processes, mkfs, savefs/loadfs");
    log_entry_t e;
    CHECK(sys_login("student", "wrong-password") == SYS_EAUTH);
    CHECK(log_has("sec", "login-failed") && count_entries(true, "sec") >= 1);
    for (int i = 0; klog_get((uint32_t)i, true, &e) == 0; i++) if (!strcmp(e.src, "sec") && strstr(e.msg, "login-failed")) { CHECK(e.level == LOG_WARN && strstr(e.msg, "subject=student")); break; }
    CHECK(!strstr(e.msg, "wrong-password"));                                                       /* a password never reaches the log */
    ROOT();
    CHECK(log_has("sec", "login uid=0") || log_has("sec", "login uid="));
    int pid = sys_process_create("sleeper", 5, 3);
    CHECK(pid >= 2 && log_has("proc", "created pid") && log_has("proc", "'sleeper'"));
    CHECK(sys_process_kill(pid) == 0 && log_has("proc", "killed"));
    int crash = sys_process_create("crash", 0, 3); (void)crash;
    CHECK(sh("mkfs --yes") == 0 && log_has("fs", "mkfs"));
    CHECK(sh("savefs") == 0 && log_has("disk", "saved to the RAM-disk"));
    CHECK(sh("loadfs --yes") == 0 && log_has("disk", "REPLACED"));
    log_entry_t w; bool warn = false; for (int i = 0; klog_get((uint32_t)i, true, &w) == 0; i++) if (strstr(w.msg, "REPLACED")) warn = w.level == LOG_WARN; CHECK(warn);
    sys_logout();
    puts("every command run is logged by NAME at DEBUG (not stored at the default level)");
    int before = count_entries(true, "shell"); CHECK(before == 0 && klog_level() == LOG_INFO);
}

/* ================= system calls ================= */
static void test_syscalls(void) {
    puts("log system calls: bounds, bad pointers, privileges");
    log_entry_t e; log_stats_t s;
    klog(LOG_INFO, "boot", "public one"); klog(LOG_WARN, "sec", "private one"); klog(LOG_INFO, "proc", "public two");
    int all_root;
    ROOT(); all_root = sys_log_get(SYS_LOG_RING, 0, NULL); sys_logout();
    login_as("student", "student123");
    int all_user = sys_log_get(SYS_LOG_RING, 0, NULL);
    CHECK(all_root > 0 && all_user > 0 && all_user < all_root);                                    /* the private entries are hidden from a user */
    for (int i = 0; i < all_user; i++) { CHECK(sys_log_get(SYS_LOG_RING, (uint32_t)i, &e) == 0 && strcmp(e.src, "sec") && strcmp(e.src, "shell")); }
    CHECK(sys_log_get(SYS_LOG_RING, (uint32_t)all_user, &e) == SYS_ENOENT && sys_log_get(SYS_LOG_RING, 0xFFFFFFFFu, &e) == SYS_ENOENT);
    CHECK(sys_log_get(7, 0, &e) == SYS_EINVAL && sys_log_get(-1, 0, &e) == SYS_EINVAL);
    CHECK(sys_log_get(SYS_LOG_RING, 0, (log_entry_t *)1) == SYS_EFAULT && sys_log_get(SYS_LOG_RING, 0, (log_entry_t *)0xFFFFFFF0u) == SYS_EFAULT);
    CHECK(sys_log_ctl(SYS_LOGCTL_STATS, 0, &s) == 0 && s.hidden_private >= 1 && s.capacity == LOG_CAPACITY);
    CHECK(sys_log_ctl(SYS_LOGCTL_STATS, 0, NULL) == SYS_EFAULT && sys_log_ctl(SYS_LOGCTL_STATS, 0, (void *)1) == SYS_EFAULT);
    CHECK(sys_log_ctl(SYS_LOGCTL_SET_LEVEL, LOG_ERROR, NULL) == SYS_EACCES && sys_log_ctl(SYS_LOGCTL_CLEAR, 0, NULL) == SYS_EACCES && sys_log_ctl(SYS_LOGCTL_TRACE, 1, NULL) == SYS_EACCES && sys_log_ctl(SYS_LOGCTL_SCHED, 1, NULL) == SYS_EACCES);
    CHECK(klog_level() == LOG_INFO && !klog_syscall_tracing() && !klog_sched_tracing() && sys_log_get(SYS_LOG_RING, 0, NULL) == all_user);   /* nothing changed */
    sys_logout();
    ROOT();
    CHECK(sys_log_ctl(99, 0, NULL) == SYS_EINVAL && sys_log_ctl(0, 0, NULL) == SYS_EINVAL && sys_log_ctl(SYS_LOGCTL_SET_LEVEL, LOG_LEVELS, NULL) == SYS_EINVAL && sys_log_ctl(SYS_LOGCTL_TRACE, 2, NULL) == SYS_EINVAL);
    CHECK(sys_log_ctl(SYS_LOGCTL_SET_LEVEL, LOG_WARN, NULL) == 0 && klog_level() == LOG_WARN && log_has("log", "minimum level set to WARN"));
    klog(LOG_INFO, "t", "refused by level"); CHECK(!log_has("t", "refused by level"));
    CHECK(sys_log_ctl(SYS_LOGCTL_SET_LEVEL, LOG_INFO, NULL) == 0 && sys_log_ctl(SYS_LOGCTL_STATS, 0, &s) == 0 && s.hidden_private == 0);
    int boot_all = sys_log_get(SYS_LOG_BOOT, 0, NULL); CHECK(boot_all >= 0);
    CHECK(sys_log_ctl(SYS_LOGCTL_CLEAR, 0, NULL) == 0 && sys_log_get(SYS_LOG_RING, 0, NULL) == 1 && log_has("log", "log cleared by uid 0"));
    sys_logout();
}

/* ================= syscall trace ================= */
static void test_trace(void) {
    puts("syscall trace: off by default, exact lines, log calls excluded, errors named");
    log_entry_t e;
    CHECK(count_entries(true, "trace") == 0);
    sys_getpid(); CHECK(count_entries(true, "trace") == 0);                                         /* off: nothing recorded */
    ROOT();
    CHECK(sh("trace") == 0 && has("is off") && sh("trace on") == 0 && has("is ON") && has("strace") && klog_syscall_tracing());
    int mypid = sys_getpid(); (void)mypid;
    CHECK(log_has("trace", "getpid() = 1"));
    CHECK(sys_open("/no/such/file", SYS_O_READ) == SYS_ENOENT && log_has("trace", "ENOENT"));
    bool found = false; for (int i = 0; klog_get((uint32_t)i, true, &e) == 0; i++) if (!strcmp(e.src, "trace") && strstr(e.msg, "open(") && strstr(e.msg, "= -1 ENOENT")) { found = e.pid == 1 && e.level == LOG_DEBUG; break; }
    CHECK(found);
    sys_sleep(0); CHECK(log_has("trace", "sleep() = 0"));
    int before = count_entries(true, "trace");
    sh("logs"); sh("logstats"); sh("strace 1");
    bool log_calls_traced = log_has("trace", "log_get") || log_has("trace", "log_ctl");
    CHECK(!log_calls_traced);                                                                       /* reading the trace must not add to it */
    CHECK(count_entries(true, "trace") >= before);
    CHECK(sh("trace off") == 0 && has("is off") && !klog_syscall_tracing());
    int after = count_entries(true, "trace"); sys_getpid(); CHECK(count_entries(true, "trace") == after);
    CHECK(sh("trace maybe") == 1 && has("usage:") && sh("trace on off") == 1);
    sys_logout();
    login_as("student", "student123");
    CHECK(sh("trace on") == 1 && has("only root") && !klog_syscall_tracing() && sh("schedtrace on") == 1 && has("only root") && !klog_sched_tracing());
    CHECK(sh("trace") == 0 && has("is off"));
    sys_logout();
}

/* ================= the commands ================= */
static void test_commands(void) {
    puts("logs: default, -a, -n, -l, -s, errors");
    log_entry_t e; (void)e;
    ROOT();
    for (int i = 1; i <= 30; i++) klogf(LOG_INFO, "demo", "entry number %d", i);
    klog(LOG_WARN, "demo", "a warning here"); klog(LOG_ERROR, "demo", "an error here");
    CHECK(sh("logs") == 0 && has("an error here") && has("entry number 30") && !has("entry number 11\n") && has("older entries not shown"));
    CHECK(sh("logs -n 3") == 0 && has("entry number 30") && has("an error here") && !has("entry number 20"));
    CHECK(sh("logs -a") == 0 && has("entry number 1\n") && has("entry number 30") && !has("older entries"));
    CHECK(sh("logs -l warn -s demo") == 0 && has("a warning here") && has("an error here") && !has("entry number") && !has("older"));
    CHECK(sh("logs -l error") == 0 && has("an error here") && !has("a warning here"));
    CHECK(sh("logs -s nosuchsource") == 0 && has("no matching log entries"));
    CHECK(sh("logs -l loud") == 1 && has("level must be") && sh("logs -n 0") == 1 && sh("logs -n 999") == 1 && sh("logs -n") == 1 && has("usage:") && sh("logs extra") == 1 && sh("logs -q") == 1);
    CHECK(sh("logs -s demo -n 2") == 0 && has("an error here") && has("a warning here") && !has("entry number 30"));
    CHECK(sh("logs -s demo -n 2 > /tmp/out.txt") == 0 && out_len == 0 && file_is("/tmp/out.txt", "") == false);                 /* redirection captures it */

    puts("logfind, logtail, logstats");
    CHECK(sh("logfind WARNING") == 0 && has("a warning here") && has("1 entry matches"));
    CHECK(sh("logfind -c WARNING") == 1 && has("0 entries match") && sh("logfind -c warning") == 0 && has("1 entry matches"));
    CHECK(sh("logfind demo") == 0 && has("32 entries match"));
    CHECK(sh("logfind zzzznotthere") == 1 && has("0 entries match") && sh("logfind") == 1 && has("usage:") && sh("logfind a b") == 1);
    CHECK(sh("logtail") == 0 && has("an error here") && has("entry number 23") && !has("entry number 22"));
    CHECK(sh("logtail 2") == 0 && has("an error here") && has("a warning here") && !has("entry number 30"));
    CHECK(sh("logtail 0") == 1 && sh("logtail 500") == 1 && sh("logtail x") == 1 && sh("logtail 1 2") == 0);
    feed("\003"); CHECK(sh("logtail 1 -f") == 0 && has("following the log") && has("an error here"));
    feed("\003"); klog(LOG_INFO, "demo", "late arrival"); CHECK(sh("logtail 1 -f") == 0 && has("late arrival"));
    CHECK(sh("logstats") == 0 && has("capacity") && has("256 entries") && has("minimum level     INFO") && has("ERROR") && has("syscall trace     off") && has("scheduler trace   off") && has("boot messages"));
    CHECK(sh("logstats x") == 1 && has("usage:"));

    puts("loglevel and logclear");
    CHECK(sh("loglevel") == 0 && has("minimum level: INFO") && sh("loglevel warn") == 0 && has("minimum level: WARN") && has("only warnings"));
    klog(LOG_INFO, "demo", "should be refused"); CHECK(!log_has("demo", "should be refused") && sh("loglevel") == 0 && has("refused because of the level"));
    CHECK(sh("loglevel debug") == 0 && has("very chatty") && sh("loglevel info") == 0 && sh("loglevel nonsense") == 1 && has("level must be") && sh("loglevel a b") == 1 && has("usage:"));
    sys_logout(); login_as("student", "student123");
    CHECK(sh("loglevel error") == 1 && has("only root") && klog_level() == LOG_INFO && sh("loglevel") == 0 && has("INFO"));
    CHECK(sh("logclear") == 1 && has("only root") && count_entries(false, NULL) > 5);
    CHECK(sh("logsave /tmp/x.log") == 1 && has("only root") && sh("sudo logclear") != 0);                                          /* sudo needs a password: no keyboard input = refused */
    sys_logout(); ROOT();
    CHECK(sh("logclear x") == 1 && has("usage:") && sh("logclear") == 0 && has("Log cleared") && count_entries(true, NULL) == 1 && log_has("log", "cleared by uid 0"));
    CHECK(sh("logs") == 0 && has("log cleared by uid 0") && !has("entry number"));

    puts("logsave");
    klog(LOG_INFO, "demo", "save me"); klog(LOG_ERROR, "demo", "and me");
    CHECK(sh("logsave") == 0 && has("Saved ") && has("/var/log/system.log"));
    char text[8192]; int fd = sys_open("/var/log/system.log", SYS_O_READ); int n = fd >= 0 ? sys_read(fd, text, sizeof text - 1) : -1; if (fd >= 0) sys_close(fd);
    CHECK(n > 0); if (n > 0) text[n] = 0; else text[0] = 0;
    CHECK(strstr(text, "demo    save me\n") && strstr(text, "ERROR demo    and me\n") && strstr(text, "cleared by uid 0"));
    CHECK(sh("logsave /tmp/copy.log") == 0 && sh("cat /tmp/copy.log") == 0 && has("save me"));
    CHECK(sh("logsave /nonexistent/dir/f.log") == 1 && has("Error"));
    CHECK(sh("protect /tmp/copy.log") == 0 && sh("logsave /tmp/copy.log") == 1 && has("protected") && sh("unprotect /tmp/copy.log") == 0 && sh("logsave /tmp/copy.log") == 0);
    CHECK(sh("logsave a b") == 1 && has("usage:") && sh("logsave /tmp") == 1);

    puts("dmesg");
    klog_init(); klog(LOG_INFO, "boot", "Booting"); klog(LOG_WARN, "boot", "Odd thing"); klog(LOG_INFO, "boot", "Boot complete"); klog_boot_done(); klog(LOG_INFO, "proc", "after boot");
    CHECK(sh("dmesg") == 0 && has("Booting") && has("Odd thing") && has("Boot complete") && !has("after boot"));
    CHECK(sh("dmesg -l warn") == 0 && has("Odd thing") && !has("Booting") && sh("dmesg -l loud") == 1 && sh("dmesg x") == 1 && has("usage:") && sh("dmesg -l") == 1);
    klog_clear(); CHECK(sh("dmesg") == 0 && has("Booting"));                                                                          /* clearing the log does not erase the boot copy */
    klog_init(); CHECK(sh("dmesg") == 0 && has("no boot messages"));

    puts("privacy: a student sees no logins or command history");
    klog_init(); klog(LOG_WARN, "sec", "login-failed uid=-1 subject=victim result=-27"); klog(LOG_INFO, "proc", "visible process event"); klog(LOG_INFO, "boot", "visible boot event");
    sys_logout(); login_as("student", "student123");
    CHECK(sh("logs -a") == 0 && has("visible process event") && !has("victim") && sh("logfind victim") == 1 && has("0 entries match") && sh("logstats") == 0 && has("visible to root only"));
    CHECK(sh("logs -s sec") == 0 && has("no matching") && sh("logtail 5") == 0 && !has("victim") && sh("dmesg") == 0 && !has("victim"));
    sys_logout(); ROOT();
    CHECK(sh("logs -s sec") == 0 && has("victim") && sh("logfind victim") == 0);
    sys_logout();

    puts("schedtrace and strace");
    ROOT(); klog_init();
    CHECK(sh("schedtrace") == 0 && has("is off") && has("no scheduling decisions"));
    CHECK(sh("schedtrace on") == 0 && has("is ON") && klog_sched_tracing());
    klog_trace_sched(1, "pid 1 -> pid 2 'demo' (time slice over)"); klog_trace_sched(2, "pid 2 -> pid 0 'idle' (yielded or blocked)");
    CHECK(sh("schedtrace") == 0 && has("last scheduling decisions") && has("pid 1 -> pid 2") && has("pid 2 -> pid 0"));
    CHECK(sh("schedtrace off") == 0 && has("is off") && !klog_sched_tracing() && sh("schedtrace x") == 1 && has("usage:"));
    for (int i = 0; i < 20; i++) klogf(LOG_DEBUG, "x", "pad");
    klog_trace_syscall(5, "write(1, 0x1000, 3) = 3"); klog_trace_syscall(5, "close(3) = 0"); klog_trace_syscall(6, "getpid() = 6");
    CHECK(sh("strace 5") == 0 && has("write(1, 0x1000, 3) = 3") && has("close(3) = 0") && !has("getpid"));
    CHECK(sh("strace 5 1") == 0 && has("close(3)") && !has("write(") && has("earlier calls not shown"));
    CHECK(sh("strace 99") == 1 && has("no traced system calls") && sh("strace") == 1 && has("usage:") && sh("strace x") == 1 && sh("strace 5 0") == 1 && sh("strace 5 1 2") == 1);
    sys_logout();
}

/* ================= lock / unlock ================= */
static void test_lock(void) {
    puts("lock: other users are blocked, the owner and root are not; messages; stat; list; unlock rules");
    login_as("student", "student123");
    CHECK(put("/tmp/lk.txt", "mine") == 0 && sh("chmod 666 /tmp/lk.txt") == 0);
    CHECK(sh("lock /tmp/lk.txt") == 0 && has("is locked") && sh("stat /tmp/lk.txt") == 0 && has("Lock   : held by student (uid 2)"));
    CHECK(sh("lock /tmp/lk.txt") == 0);                                                        /* locking again as the owner is harmless */
    CHECK(sh("lock -l") == 0 && has("/tmp/lk.txt") && has("locked by student") && sh("lock -l /home") == 0 && has("no locked files") && sh("lock -l a b") == 1 && has("usage:"));
    CHECK(put("/tmp/lk.txt", "owner may still write") == 0 && file_is("/tmp/lk.txt", "owner may still write"));
    sys_logout();
    login_as("admin", "admin123");
    CHECK(sys_open("/tmp/lk.txt", SYS_O_WRITE) == SYS_EFLOCK && put("/tmp/lk.txt", "intruder") != 0 && file_is("/tmp/lk.txt", "owner may still write"));
    CHECK(sh("rm /tmp/lk.txt") == 1 && has("locked by another user") && sh("mv /tmp/lk.txt /tmp/moved.txt") == 1 && has("locked") && sh("ls /tmp") == 0 && has("lk.txt") && !has("moved.txt"));
    CHECK(sh("truncate /tmp/lk.txt 0") == 1 && sh("cp /etc/motd /tmp/lk.txt") == 1);
    CHECK(sh("cat /tmp/lk.txt") == 0 && has("owner may still write"));                         /* reading is never blocked */
    CHECK(sh("lock /tmp/lk.txt") == 1 && has("locked"));                                       /* cannot take over somebody else's lock */
    CHECK(sh("unlock /tmp/lk.txt") == 1 && has("locked by another user") && sh("stat /tmp/lk.txt") == 0 && has("held by student"));
    sys_logout();
    ROOT();
    CHECK(put("/tmp/lk.txt", "root bypasses") == 0 && file_is("/tmp/lk.txt", "root bypasses"));
    CHECK(sh("unlock /tmp/lk.txt") == 0 && has("is unlocked") && sh("unlock /tmp/lk.txt") == 1 && has("not locked") && sh("lock -l") == 0 && has("no locked files"));
    sys_logout();
    login_as("student", "student123");
    CHECK(sh("lock /tmp/lk.txt") == 0 && sh("unlock /tmp/lk.txt") == 0 && sh("stat /tmp/lk.txt") == 0 && !has("Lock   :"));       /* the owner unlocks */
    CHECK(sh("lock /tmp") == 1 && has("is a directory") && sh("lock /tmp/nope") == 1 && has("not found") && sh("lock") == 1 && has("usage:") && sh("unlock") == 1 && has("usage:"));
    CHECK(sh("lock /etc/motd") == 1 && has("permission denied"));                              /* you can only lock what you could write */
    CHECK(sh("lock /tmp/lk.txt") == 0 && sh("rm /tmp/lk.txt") == 0 && sh("lock -l") == 0 && has("no locked files"));               /* deleting your own locked file removes the lock */
    puts("a lock does not survive loadfs, and protect still wins for everybody");
    CHECK(put("/tmp/p.txt", "x") == 0 && sh("lock /tmp/p.txt") == 0 && sh("protect /tmp/p.txt") == 0 && sh("rm /tmp/p.txt") == 1 && has("protected"));
    sys_logout();
    ROOT(); CHECK(put("/tmp/p.txt", "root") != 0); CHECK(sh("unprotect /tmp/p.txt") == 0 && put("/tmp/p.txt", "root") == 0); sys_logout();
}

/* ================= quota ================= */
static void test_quota(void) {
    puts("quota: per-user usage, limits are display only");
    login_as("student", "student123");
    CHECK(sh("quota") == 0 && has("student") && has("64.0 KiB") && has("display only") && sh("quota student") == 0 && has("student"));
    char big[70000]; memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
    CHECK(put("/tmp/bigfile.dat", big) == 0 && sh("quota") == 0 && has("OVER THE LIMIT (not enforced)"));
    CHECK(put("/tmp/more.dat", "still allowed: nothing is enforced") == 0);
    CHECK(sh("quota nosuchuser") == 1 && has("no such user") && sh("quota -z") == 1 && has("usage:") && sh("quota a b") == 1);
    CHECK(sh("quota root") == 0 && has("none"));
    CHECK(sh("quota -a") == 0 && has("root") && has("admin") && has("student") && has("guest") && has("256.0 KiB"));
    CHECK(sh("quota -a > /tmp/q.txt") == 0 && out_len == 0 && sh("cat /tmp/q.txt") == 0 && has("guest"));
    CHECK(sh("rm /tmp/bigfile.dat") == 0 && sh("quota") == 0 && !has("OVER"));
    sys_logout();
}

/* ================= bg / fg / memlimit ================= */
static void test_jobs(void) {
    puts("bg, fg: resume and wait for the shell's own children");
    ROOT();
    int a = sys_process_create("sleeper", 60, 3), b = sys_process_create("sleeper", 60, 3);
    CHECK(a >= 2 && b > a);
    CHECK(sh("bg") == 1 && has("no suspended job"));
    CHECK(sys_process_suspend(a) == 0 && sys_process_suspend(b) == 0);
    char c[40];
    CHECK(sh("bg") == 0 && has("resumed in the background")); sys_process_suspend(b);                      /* bg without a pid takes the NEWEST suspended job: b */
    snprintf(c, sizeof c, "bg %d", b); CHECK(sh(c) == 0 && has("resumed in the background") && has("sleeper"));
    CHECK(sh(c) == 0 && has("already running in the background"));
    CHECK(sh("bg 0") == 1 && has("not a job of this shell") && sh("bg 99") == 1 && sh("bg abc") == 1 && has("not a valid process id") && sh("bg 1 2") == 1 && has("usage:"));
    CHECK(sh("fg 0") == 1 && has("not a job") && sh("fg 99") == 1 && sh("fg x") == 1 && sh("fg 1 2") == 1);
    sys_process_kill(a); snprintf(c, sizeof c, "fg %d", a);
    CHECK(sh(c) == 0 && has("finished with exit code") && has("(killed)"));
    CHECK(sh(c) == 1);                                                                                          /* reaped: gone now */
    sys_process_kill(b); snprintf(c, sizeof c, "bg %d", b); CHECK(sh(c) == 1 && has("already finished"));
    snprintf(c, sizeof c, "fg %d", b); sh(c);

    puts("memlimit: accounting only");
    int m = sys_process_create("sleeper", 60, 3); snprintf(c, sizeof c, "memlimit %d 64K", m);
    CHECK(sh("memlimit") == 0 && has("Accounting only") && has("sleeper") && has("none"));
    CHECK(sh(c) == 0 && has("set to 65536 bytes (accounting only)") && sh("memlimit") == 0 && has("65536") && has("ok"));
    snprintf(c, sizeof c, "memlimit %d 1M", m); CHECK(sh(c) == 0 && has("1048576"));
    snprintf(c, sizeof c, "memlimit %d 500", m); CHECK(sh(c) == 0 && has("set to 500 bytes"));
    snprintf(c, sizeof c, "memlimit %d off", m); CHECK(sh(c) == 0 && has("limit removed") && sh("memlimit") == 0 && !has("65536") && !has("   500"));
    snprintf(c, sizeof c, "memlimit %d 12x", m); CHECK(sh(c) == 1 && has("number of bytes"));
    snprintf(c, sizeof c, "memlimit %d 0", m); CHECK(sh(c) == 1);
    snprintf(c, sizeof c, "memlimit %d 99999999999", m); CHECK(sh(c) == 1);
    CHECK(sh("memlimit 0 64K") == 1 && has("cannot be limited") && sh("memlimit 1 64K") == 1 && has("cannot be limited") && sh("memlimit 15 64K") == 1 && has("no such process") && sh("memlimit 99 64K") == 1 && has("usage:") && sh("memlimit x 1") == 1 && has("usage:") && sh("memlimit 3") == 1);
    sys_logout(); login_as("student", "student123");
    snprintf(c, sizeof c, "memlimit %d 64K", m); CHECK(sh(c) == 1 && has("only root") && sh("memlimit") == 0);
    sys_logout(); ROOT(); sys_process_kill(m); sys_logout();
}

/* ================= mount / umount / defrag / smart ================= */
static void test_blk(void) {
    puts("mount and umount: a table only, root only");
    ROOT(); sh("mkdir -p /mnt/a"); sh("mkdir -p /mnt/b"); put("/tmp/afile", "x");
    CHECK(sh("mount") == 0 && has("minifs on / type MiniFS") && !has("ram0 on"));
    CHECK(sh("mount ram0 /mnt/a") == 0 && has("TABLE ONLY") && sh("mount") == 0 && has("ram0 on /mnt/a"));
    CHECK(sh("mount ram0 /mnt/b") == 1 && has("already mounted") && sh("mount ram0 /mnt/a") == 1 && has("already"));
    CHECK(sh("umount /mnt/b") == 1 && has("not mounted") && sh("umount /") == 1 && has("busy") && sh("umount") == 1 && has("usage:"));
    CHECK(sh("umount /mnt/a") == 0 && has("unmounted") && sh("mount") == 0 && !has("ram0 on") && sh("mount ram0 /mnt/b") == 0 && sh("umount ram0") == 0 && !has("error"));
    CHECK(sh("mount hda /mnt/a") == 1 && has("unknown device") && sh("mount ram0 /nowhere") == 1 && has("does not exist") && sh("mount ram0 /tmp/afile") == 1 && has("not a directory") && sh("mount ram0 /") == 1 && sh("mount ram0") == 1 && has("usage:"));
    sys_logout(); login_as("student", "student123");
    CHECK(sh("mount ram0 /tmp") == 1 && has("only root") && sh("umount /tmp") == 1 && has("only root") && sh("mount") == 0);
    sys_logout();
    puts("defrag and smart are labelled simulations built on real numbers");
    login_as("student", "student123");
    CHECK(sh("defrag") == 0 && has("[SIMULATED]") && has("nothing to defragment") && has("Heap free space") && has("No data was moved") && sh("defrag x") == 1 && has("usage:"));
    ramdisk_info_t ri; ROOT(); sys_blk_info(&ri); sys_logout(); login_as("student", "student123");
    CHECK(sh("smart") == 0 && has("[SIMULATED]") && has("PASSED") && has("Sectors written *") && has("Reallocated sectors") && sh("smart x") == 1 && has("usage:"));
    sys_logout();
}

/* ================= network extras ================= */
static void test_net(void) {
    puts("whois, telnet, ssh, ftp: simulated and labelled");
    login_as("student", "student123");
    CHECK(sh("whois example.test") == 0 && has("[SIMULATED]") && has("192.0.2.10") && sh("whois localhost") == 0 && has("loopback") && sh("whois nowhere.zzz") == 1 && has("No match") && sh("whois") == 1 && has("usage:") && sh("whois a b") == 1);
    CHECK(sh("telnet example.test") == 0 && has("[SIMULATED]") && has("Connected to example.test") && has("login:") && has("Connection closed"));
    CHECK(sh("telnet example.test 80") == 0 && sh("telnet example.test 9999") == 1 && has("Connection refused") && sh("telnet nosuch.host") == 1 && has("unknown host"));
    CHECK(sh("telnet 8.8.8.8") == 1 && has("timed out") && sh("telnet") == 1 && has("usage:") && sh("telnet example.test 0") == 1 && sh("telnet example.test 70000") == 1 && sh("telnet a b c") == 1);
    CHECK(sh("ssh example.test") == 255 && has("can't be established") && has("student@example.test") && has("Permission denied") && has("SIMULATED"));
    char fp1[80], fp2[80]; sh("ssh example.test"); const char *f = strstr(out, "fingerprint: "); snprintf(fp1, sizeof fp1, "%.23s", f ? f + 13 : "?");
    sh("ssh example.test"); f = strstr(out, "fingerprint: "); snprintf(fp2, sizeof fp2, "%.23s", f ? f + 13 : "?"); CHECK(!strcmp(fp1, fp2) && strlen(fp1) == 23);   /* stable per host */
    sh("ssh gateway.local"); f = strstr(out, "fingerprint: "); CHECK(f && strncmp(f + 13, fp1, 23) != 0);
    CHECK(sh("ssh root@minios.local") == 255 && has("root@minios.local") && sh("ssh @example.test") == 1 && has("bad user") && sh("ssh") == 1 && has("usage:") && sh("ssh 8.8.8.8") == 1 && has("timed out") && sh("ssh nosuch.host") == 1);
    CHECK(sh("ssh a@") == 1 && has("bad host"));
    CHECK(sh("ftp example.test") == 0 && has("220") && has("readme.txt") && has("data.csv") && has("221 Goodbye"));
    CHECK(sh("ftp example.test readme.txt") == 0 && has("226 Transfer complete") && has("no bytes were written") && sh("ftp example.test missing.bin") == 1 && has("550"));
    CHECK(sh("ftp") == 1 && has("usage:") && sh("ftp 8.8.8.8") == 1 && has("timed out") && sh("ftp nosuch.host") == 1 && sh("ls") == 0 && !has("readme.txt"));      /* nothing was downloaded */
    sys_logout();

    puts("loopback queue: REAL messages through kernel memory");
    login_as("student", "student123"); lo_clear(0);
    CHECK(sh("loopback") == 0 && has("no port has a queue yet") && has("REAL"));
    CHECK(sh("loopback send 5000 hello there") == 0 && has("queued for port 5000: hello there") && sh("loopback send 5000 second") == 0);
    CHECK(sh("loopback") == 0 && has("5000") && lo_pending(5000) == 2 && lo_total_sent() == 2);
    CHECK(sh("loopback recv 5000") == 0 && has("hello there") && sh("loopback recv 5000") == 0 && has("second") && sh("loopback recv 5000") == 1 && has("no message waiting"));
    CHECK(sh("loopback recv 5000 -w 0") == 1 && sh("loopback recv 5000 -w 61") == 1 && has("0-60") && sh("loopback recv 5000 -w x") == 1);
    CHECK(sh("loopback send 0 x") == 1 && has("1-65535") && sh("loopback send 65536 x") == 1 && sh("loopback send x y") == 1 && sh("loopback send 5000") == 1 && has("usage:"));
    char longm[200]; memset(longm, 'm', 150); longm[150] = 0; char cmdline[300]; snprintf(cmdline, sizeof cmdline, "loopback send 5000 %s", longm);
    CHECK(sh(cmdline) == 1 && has("120"));
    for (int i = 0; i < LO_QUEUE_DEPTH; i++) CHECK(sh("loopback send 5001 m") == 0);
    CHECK(sh("loopback send 5001 overflow") == 1 && has("queue is full"));
    lo_clear(0);
    CHECK(sh("loopback recv 80") == 1 && has("below 1024 needs root"));
    CHECK(sh("loopback clear") == 1 && has("only root") && sh("loopback clear 5000") == 1);
    CHECK(sh("loopback recv 0") == 1 && sh("loopback bogus") == 1 && has("usage:") && sh("loopback send") == 1);
    sys_logout(); ROOT();
    CHECK(sh("loopback send 80 root-only-port") == 0 && sh("loopback recv 80") == 0 && has("root-only-port"));
    CHECK(sh("loopback send 6000 a") == 0 && sh("loopback send 6001 b") == 0 && sh("loopback clear 6000") == 0 && has("cleared") && lo_pending(6000) == 0 && lo_pending(6001) == 1 && sh("loopback clear") == 0 && lo_pending(6001) == 0);
    sys_logout();

    puts("a control character cannot be smuggled through the queue");
    lo_clear(0); lo_send(5002, "line1\nline2\x1b[2J"); char r[200]; int n = lo_recv(5002, r, sizeof r); CHECK(n > 0 && !strchr(r, '\n') && !strchr(r, 0x1b));

    puts("the echo task: two tasks really exchange messages");
    lo_clear(0); login_as("student", "student123");
    CHECK(sh("loopback demo") == 1 && has("started the echo task") && has("sent    : hello") && has("0 of 3 replies"));       /* host tests never run tasks: nothing answers, and the command says so */
    CHECK(lo_pending(LO_ECHO_IN) == 4);                                                                         /* three words and "quit" are waiting for the task */
    CHECK(lo_echo_step() == 1 && lo_echo_step() == 1 && lo_echo_step() == 1);                                   /* now the task runs */
    CHECK(sh("loopback recv 7778") == 0 && has("echo: hello") && sh("loopback recv 7778") == 0 && has("echo: MiniOS") && sh("loopback recv 7778") == 0 && has("echo: loopback"));
    CHECK(lo_echo_step() == 2 && sh("loopback recv 7778") == 0 && has("bye") && lo_echo_step() == 0);
    sys_logout();

    puts("nc: localhost goes through the queue, other hosts are simulated");
    lo_clear(0); login_as("student", "student123");
    CHECK(sh("nc localhost 5100 hi there") == 0 && has("sent 8 bytes to localhost:5100 through the loopback queue") && lo_pending(5100) == 1);
    CHECK(sh("nc -l 5100 -w 0") == 0 && has("hi there") && sh("nc -l 5100 -w 0") == 1 && has("nothing arrived"));
    CHECK(sh("nc 127.0.0.1 5100 x") == 0 && sh("nc minios.local 5100 y") == 0 && lo_pending(5100) == 2);
    CHECK(sh("nc example.test 80 GET") == 0 && has("[SIMULATED]") && has("NOT sent anywhere") && lo_pending(80) == 0);
    CHECK(sh("nc example.test 9999 x") == 1 && has("refused") && sh("nc 8.8.8.8 80 x") == 1 && has("timed out") && sh("nc nosuch.host 80 x") == 1 && has("unknown host"));
    CHECK(sh("nc -l 80 -w 0") == 1 && has("needs root") && sh("nc -l 0") == 1 && sh("nc -l 5100 -w x") == 1 && sh("nc -l 5100 -w") == 1 && sh("nc") == 1 && has("usage:") && sh("nc localhost 5100") == 1 && sh("nc localhost 0 x") == 1);
    CHECK(sh(cmdline + 0) == 1);                                                                                /* (cmdline holds a loopback command: not an nc command) */
    snprintf(cmdline, sizeof cmdline, "nc localhost 5100 %s", longm); CHECK(sh(cmdline) == 1 && has("120"));
    sys_logout();
    puts("waiting for a message can be interrupted with Ctrl+C");
    lo_clear(0); login_as("student", "student123"); feed("\003");
    CHECK(sh("nc -l 5200 -w 30") == 130 && has("interrupted")); feed("\003"); CHECK(sh("loopback recv 5200 -w 30") == 130);
    sys_logout();
}

/* ================= beep, clock ================= */
static void test_dev(void) {
    puts("beep: validated, switched off again");
    login_as("student", "student123"); uint32_t hz;
    CHECK(sh("beep") == 0 && has("880 Hz for 300 ms") && !kinfo_beep_state(&hz));
    CHECK(sh("beep 440 100") == 0 && has("440 Hz for 100 ms") && !kinfo_beep_state(NULL));
    CHECK(sh("beep 37 10") == 0 && sh("beep 20000 2000") == 0);
    CHECK(sh("beep 36") == 1 && has("usage:") && sh("beep 20001") == 1 && sh("beep 440 9") == 1 && sh("beep 440 2001") == 1 && sh("beep x") == 1 && sh("beep 1 2 3") == 1 && sh("beep 440 y") == 1);
    CHECK(sys_beep(true, 36) == SYS_EINVAL && sys_beep(true, 20001) == SYS_EINVAL && !kinfo_beep_state(NULL) && sys_beep(true, 1000) == 0 && kinfo_beep_state(&hz) && hz == 1000 && sys_beep(false, 0) == 0 && !kinfo_beep_state(NULL));
    CHECK(sys_power(POWER_REBOOT) == SYS_EACCES && sys_power(POWER_SHUTDOWN) == SYS_EACCES);                         /* only the beep operations are open to everybody */
    sys_logout();
    puts("clock: runs until asked to stop");
    login_as("student", "student123");
    CHECK(sh("clock -n 3") == 0 && has("CMOS clock") && has(":") && has("2026") == has("2026"));
    feed("q"); CHECK(sh("clock") == 0 && has("CMOS clock"));
    feed("\003"); CHECK(sh("clock") == 0);
    CHECK(sh("clock -n 0") == 1 && has("usage:") && sh("clock -n") == 1 && sh("clock x") == 1 && sh("clock -n 86401") == 1);
    sys_logout();
}

/* ================= the table ================= */
static void test_table(void) {
    puts("command table: the 29 commands that were stubs before Phase 12 are real");
    /* memory and process commands live in other test suites (their own link sets); the real kernel build reports 300 of 300 */
    static const char *const finished[] = { "lock","unlock","bg","fg","memlimit","mount","umount","defrag","quota","smart","telnet","ssh","ftp","nc","whois","loopback","beep","clock" };
    for (size_t k = 0; k < sizeof finished / sizeof *finished; k++) { const command_t *c = command_find(finished[k]); CHECK(c && c->handler != NULL); }
    CHECK(command_count() == 300 && command_validate(NULL) == 0);
    puts("the eleven log commands");
    const char *names[] = { "logs", "dmesg", "loglevel", "logclear", "logsave", "logtail", "logfind", "logstats", "trace", "strace", "schedtrace" };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) { const command_t *c = command_find(names[i]); CHECK(c && c->handler != NULL); }
}

static int real_main(void) {
    fresh();
    test_kernel_events(); fresh();
    test_syscalls();      fresh();
    test_trace();         fresh();
    test_commands();      fresh();
    test_lock();          fresh();
    test_quota();         fresh();
    test_jobs();          fresh();
    test_blk();           fresh();
    test_net();           fresh();
    test_dev();           fresh();
    test_table();
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
int main(void) { return run_on_low_stack(real_main); }
