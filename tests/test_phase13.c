/* Phase 13 tests: pipelines (cmd | cmd | cmd > file). Everything runs through the REAL parser, pipeline runner, output capture,
 * system calls, MiniFS and the real commands. Only the screen, keyboard, timer and context switch are stubbed. */
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
#include "include/pipeline.h"
#include "include/cmdutil.h"
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
static const command_t *lookup(const char *name) { return command_find(name); }
static int sh(const char *raw) {                     /* same pipeline as shell/shell.c: aliases, $variables, parse, pipeline, run */
    static char buf[SHELL_LINE_MAX * 2], aliased[SHELL_LINE_MAX * 2], line[SHELL_LINE_MAX * 2]; pipeline_t pl; bool was;
    out_reset();
    if (sv_expand_alias(raw, aliased, sizeof aliased, &was) != SV_OK || sv_expand_vars(aliased, line, sizeof line) != SV_OK) { screen_puts("Error: too long\n"); return 2; }
    int rc = parse_pipeline(line, buf, sizeof buf, &pl);
    if (rc == PARSE_EMPTY) return 0;
    if (rc != PARSE_OK) { screen_puts("Error: "); screen_puts(parse_error_string(rc)); screen_putc('\n'); return 2; }
    int st = pipeline_run(&pl, lookup, owner_yes);
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


static bool is(const char *want) { return strcmp(out, want) == 0; }
static int shell_entries(const char *prefix) {            /* DEBUG "shell" entries whose text starts with prefix */
    int n = 0; log_entry_t e;
    for (int i = 0; klog_get((uint32_t)i, true, &e) == 0; i++) if (!strcmp(e.src, "shell") && !strncmp(e.msg, prefix, strlen(prefix))) n++;
    return n;
}
static void make_files(void) {
    put("/tmp/names.txt", "pear\napple\nfig\napple\nbanana\n");
    put("/tmp/nums.csv", "ada,90\nlinus,85\ngrace,95\n");
    put("/tmp/text.txt", "the cat and the dog\nthe end\n");
}

/* ================= basics ================= */
static void test_basics(void) {
    puts("two stages: the output of the first is the input of the second");
    ROOT(); make_files();
    CHECK(sh("cat /tmp/names.txt | sort") == 0 && is("apple\napple\nbanana\nfig\npear\n"));
    CHECK(sh("cat /tmp/names.txt | sort -r") == 0 && is("pear\nfig\nbanana\napple\napple\n"));
    CHECK(sh("cat /tmp/names.txt | tac") == 0 && is("banana\napple\nfig\napple\npear\n"));
    CHECK(sh("cat /tmp/names.txt | rev") == 0 && has("raep") && has("elppa"));
    CHECK(sh("cat /tmp/names.txt | nl") == 0 && has("     1  pear") && has("     5  banana"));
    CHECK(sh("cat /tmp/names.txt | head -n 2") == 0 && is("pear\napple\n"));
    CHECK(sh("cat /tmp/names.txt | tail -n 1") == 0 && is("banana\n"));
    CHECK(sh("cat /tmp/names.txt | wc -l") == 0 && has("5"));
    CHECK(sh("cat /tmp/text.txt | grep cat") == 0 && is("the cat and the dog\n"));
    CHECK(sh("cat /tmp/text.txt | grep -i THE") == 0 && has("the cat") && has("the end"));
    CHECK(sh("cat /tmp/nums.csv | cut -d , -f 1") == 0 && is("ada\nlinus\ngrace\n"));
    CHECK(sh("cat /tmp/names.txt | tr a-z A-Z") == 0 && is("PEAR\nAPPLE\nFIG\nAPPLE\nBANANA\n"));
    CHECK(sh("cat /tmp/names.txt | upper") == 0 && is("PEAR\nAPPLE\nFIG\nAPPLE\nBANANA\n"));
    CHECK(sh("cat /tmp/names.txt | capitalize") == 0 && is("Pear\nApple\nFig\nApple\nBanana\n"));
    CHECK(sh("cat /tmp/text.txt | wordfreq") == 0 && has("the") && has("3"));
    CHECK(sh("echo AB | hexdump") == 0 && has("41 42"));
    CHECK(sh("cat /tmp/names.txt | join ,") == 0 && is("pear,apple,fig,apple,banana\n"));
    CHECK(sh("cat /tmp/names.txt | fold -w 4") == 0 && has("bana\nna"));
    CHECK(sh("ls /etc | grep passwd") == 0 && has("passwd"));
    CHECK(sh("echo hello | cat") == 0 && is("hello\n"));
    CHECK(sh("echo b a c | sort") == 0 && is("b a c\n"));

    puts("three and four stages");
    CHECK(sh("cat /tmp/names.txt | sort | uniq") == 0 && is("apple\nbanana\nfig\npear\n"));
    CHECK(sh("cat /tmp/names.txt | sort | uniq -c") == 0 && has("2 apple") && has("1 pear"));
    CHECK(sh("cat /tmp/names.txt | sort | uniq | wc -l") == 0 && has("4"));
    CHECK(sh("cat /tmp/names.txt | sort | uniq | tac") == 0 && is("pear\nfig\nbanana\napple\n"));
    CHECK(sh("cat /tmp/text.txt | grep the | wc -l | cat") == 0 && has("2"));
    CHECK(sh("cat /tmp/names.txt | sort | uniq -c | sort -n") == 0 && has("2 apple"));

    puts("numbers: sum, min, max and avg read a pipe");
    CHECK(sh("cat /tmp/nums.csv | cut -d , -f 2 | sum") == 0 && is("270\n"));
    CHECK(sh("cat /tmp/nums.csv | cut -d , -f 2 | min") == 0 && is("85\n"));
    CHECK(sh("cat /tmp/nums.csv | cut -d , -f 2 | max") == 0 && is("95\n"));
    CHECK(sh("cat /tmp/nums.csv | cut -d , -f 2 | avg") == 0 && is("90.00\n"));
    CHECK(sh("sum -f /tmp/nums.csv") == 1 && has("not a number"));                       /* the file form works without a pipe too: names are not numbers */
    CHECK(put("/tmp/only.txt", "1 2, 3\n4\n") == 0 && sh("sum -f /tmp/only.txt") == 0 && is("10\n"));
    CHECK(sh("echo 7 8 9 | sum") == 0 && is("24\n") && sh("echo | sum") == 1 && has("no numbers"));
    CHECK(sh("sum") == 1 && has("usage:") && has("sum -f <file>"));
    sys_logout();
}

/* ================= redirection of the whole pipeline ================= */
static void test_redirect(void) {
    puts("> and >> apply to the LAST command only");
    ROOT(); make_files();
    CHECK(sh("cat /tmp/names.txt | sort | uniq > /tmp/out.txt") == 0 && out_len == 0 && file_is("/tmp/out.txt", "apple\nbanana\nfig\npear\n"));
    CHECK(sh("cat /tmp/nums.csv | cut -d , -f 2 >> /tmp/out.txt") == 0 && file_is("/tmp/out.txt", "apple\nbanana\nfig\npear\n90\n85\n95\n"));
    CHECK(sh("echo x | cat > /tmp/new.txt") == 0 && file_is("/tmp/new.txt", "x\n"));
    CHECK(sh("cat /tmp/out.txt | wc -l > /tmp/count.txt") == 0 && sh("cat /tmp/count.txt") == 0 && has("7"));
    CHECK(sh("cat /tmp/names.txt > /tmp/a | sort") == 2 && has("only the last command") && sh("ls /tmp") == 0 && !has("a\n"));
    CHECK(sh("cat /tmp/names.txt | sort > /nonexistent/x") == 1 && has("cannot redirect") && shell_entries("cat") == 0);          /* checked BEFORE anything ran */
    CHECK(sh("cat /tmp/names.txt | sort > /tmp") == 1 && has("cannot redirect"));
    puts("a failing pipeline leaves the target file alone");
    put("/tmp/keep.txt", "KEEP\n");
    CHECK(sh("cat /tmp/names.txt | cut > /tmp/keep.txt") == 1 && has("usage:") && file_is("/tmp/keep.txt", "KEEP\n"));
    sys_logout();
}

/* ================= parse errors and checks before running ================= */
static void test_errors(void) {
    puts("parse errors");
    ROOT(); make_files();
    CHECK(sh("cat /tmp/names.txt |") == 2 && has("missing command next to '|'") && sh("| sort") == 2 && has("missing command") && sh("cat /tmp/names.txt || sort") == 2);
    CHECK(sh("a | b | c | d | e") == 2 && has("at most 4") && sh("echo 'a | b") == 2 && has("unterminated"));
    CHECK(sh("cat /tmp/names.txt | | sort") == 2);
    puts("everything is checked BEFORE any stage runs");
    klog_set_level(LOG_DEBUG); int before = shell_entries("cat");
    CHECK(sh("cat /tmp/names.txt | nosuchcommand") == 127 && has("unknown command 'nosuchcommand'") && shell_entries("cat") == before);
    CHECK(sh("nosuchcommand | sort") == 127 && has("unknown command"));
    CHECK(sh("cat /tmp/names.txt | pwd") == 1 && has("does not read from a pipe") && has("grep") && shell_entries("cat") == before);
    CHECK(sh("cat /tmp/names.txt | ls") == 1 && has("'ls' does not read from a pipe") && shell_entries("cat") == before);
    CHECK(sh("cat /tmp/names.txt | sudo ls") == 1 && has("cannot be part of a pipeline") && sh("sudo ls | cat") == 1 && has("'sudo'"));
    CHECK(sh("echo x | login") == 1 && sh("echo x | su") == 1 && sh("echo x | passwd") == 1 && sh("echo x | exit") == 1 && has("'exit'") && sh("exit | cat") == 1 && sh("logout | cat") == 1);
    CHECK(sh("sudo ls") != 2 && !has("cannot be part of a pipeline"));                         /* the single-command forms are unchanged */
    puts("a stage with a command argument limit");
    CHECK(sh("cat /tmp/names.txt | grep a b c d e f g h i j k l m n o p") == 2 && has("too many arguments"));
    CHECK(sh("cat /tmp/names.txt | grep a b c d e f g h i j k l m n o") == 1 && has("too many arguments"));
    CHECK(sh("cat /tmp/names.txt | sum 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15") == 1 && has("too many arguments"));
    sys_logout();
}

/* ================= the '-' file name ================= */
static void test_dash(void) {
    puts("'-' alone, without a pipe, says so; the pipe input never leaks to a later command");
    ROOT(); make_files();
    CHECK(sh("sort -") == 1 && has("no input") && has("cmd1 | cmd2"));
    CHECK(sh("cat -") == 1 && has("no input") && sh("wc -") == 1 && sh("head -n 1 -") == 1);
    CHECK(sh("cat /tmp/names.txt | sort") == 0 && sh("sort -") == 1 && has("no input"));                      /* cleared after a successful pipeline */
    CHECK(sh("cat /tmp/missing | sort") == 1 && sh("sort -") == 1 && has("no input"));                        /* cleared after a failed one */
    CHECK(cu_pipe_active() == false);
    puts("'-' placed by hand, for commands the shell does not feed automatically");
    CHECK(sh("cat /tmp/names.txt | paste /tmp/nums.csv -") == 0 && has("ada,90\tpear") && has("grace,95\tfig"));
    CHECK(sh("cat /tmp/names.txt | cmp - /tmp/names.txt") == 0);
    CHECK(sh("cat /tmp/names.txt | cmp /tmp/names.txt /tmp/nums.csv -") != 0);
    CHECK(sh("cat /tmp/names.txt | diff /tmp/names.txt -") == 0 && has("identical"));
    CHECK(sh("cat /tmp/nums.csv | diff /tmp/names.txt -") != 0 && has("< pear") && has("> ada,90"));
    CHECK(sh("cat /tmp/names.txt | sort - /tmp/nums.csv") != 0);                                              /* sort takes one file: the explicit '-' is respected, no second '-' added */
    puts("the same buffer can be read twice in one command");
    CHECK(sh("cat /tmp/names.txt | cmp - -") == 0);
    sys_logout();
}

/* ================= failures inside a pipeline ================= */
static void test_failures(void) {
    puts("a failing first stage is reported and stops the pipeline");
    ROOT(); make_files();
    CHECK(sh("cat /tmp/missing | sort") == 1 && has("Error:") && has("(pipeline stopped: 'cat' failed)") && !has("apple"));
    CHECK(sh("cat /tmp/missing | wc -l") == 1 && has("Error:") && has("pipeline stopped") && !has("0 "));
    CHECK(sh("cut | sort") == 1 && has("usage:") && has("pipeline stopped: 'cut'"));
    CHECK(sh("cat /tmp/names.txt | cut | sort") == 1 && has("usage:") && has("pipeline stopped: 'cut'") && !has("apple"));
    puts("finding nothing is not a failure");
    CHECK(sh("cat /tmp/names.txt | grep zzz | wc -l") == 0 && has("0") && !has("stopped"));
    CHECK(sh("cat /tmp/names.txt | grep zzz") == 1 && out_len == 0);                                             /* status 1, as grep always did */
    CHECK(sh("cat /tmp/names.txt | grep zzz | cat") == 0 && out_len == 0);
    puts("the exit status of the pipeline is the status of its LAST command");
    CHECK(sh("cat /tmp/names.txt | grep zzz") == 1 && sh("echo $?") == 0 && is("1\n"));
    CHECK(sh("cat /tmp/missing | sort") == 1 && sh("echo $?") == 0 && is("1\n"));
    CHECK(sh("cat /tmp/names.txt | sort") == 0 && sh("echo $?") == 0 && is("0\n"));
    CHECK(sh("cat /tmp/names.txt | contains") == 1 && has("'contains' does not read from a pipe"));
    sys_logout();
}

/* ================= limits ================= */
static void test_limits(void) {
    puts("a stage may produce up to 32 KiB");
    ROOT();
    char big[40000]; for (size_t i = 0; i < sizeof big - 1; i++) big[i] = (i % 80 == 79) ? '\n' : 'a'; big[sizeof big - 1] = 0;
    CHECK(put("/tmp/big.txt", big) == 0);
    CHECK(sh("cat /tmp/big.txt | wc -c") == 1 && has("larger than 32 KiB") && has("'cat'"));
    CHECK(sh("cat /tmp/big.txt | wc -l") == 1 && has("larger than 32 KiB"));
    char ok[30001]; for (size_t i = 0; i < 30000; i++) ok[i] = (i % 80 == 79) ? '\n' : 'b'; ok[30000] = 0;
    CHECK(put("/tmp/ok.txt", ok) == 0 && sh("cat /tmp/ok.txt | wc -c") == 0 && has("30000"));
    CHECK(sh("cat /tmp/ok.txt | wc -l") == 0 && has("375"));
    puts("many pipelines in a row do not leak memory or process allocations");
    memory_summary_t m0, m1; memory_summary(&m0);
    make_files();
    for (int i = 0; i < 120; i++) { CHECK(sh("cat /tmp/names.txt | sort | uniq | wc -l") == 0); if (i % 40 == 0 && !has("4")) { CHECK(false); break; } }
    for (int i = 0; i < 60; i++) { if (sh("cat /tmp/missing | sort") != 1) { CHECK(false); break; } }
    for (int i = 0; i < 60; i++) { if (sh("cat /tmp/big.txt | wc -c") != 1) { CHECK(false); break; } }
    memory_summary(&m1); CHECK(m1.free_bytes == m0.free_bytes);
    sys_logout();
}

/* ================= security ================= */
static void test_security(void) {
    puts("a pipe never bypasses permissions: every command still reads through the system calls");
    ROOT(); put("/tmp/secret.txt", "TOP SECRET\n"); sh("chmod 600 /tmp/secret.txt"); make_files(); sys_logout();
    login_as("student", "student123");
    CHECK(sh("cat /tmp/secret.txt | sort") == 1 && has("ermission denied") && !has("TOP SECRET"));
    CHECK(sh("cat /tmp/secret.txt | sort > /tmp/leak.txt") == 1 && !has("TOP SECRET"));
    CHECK(sh("cat /tmp/names.txt | sort > /tmp/student-out.txt") == 0 && file_is("/tmp/student-out.txt", "apple\napple\nbanana\nfig\npear\n"));
    CHECK(sh("cat /tmp/names.txt | sort > /etc/passwd") == 1 && has("cannot redirect") && shell_entries("cat") == 0);
    CHECK(sh("cat /tmp/names.txt | sort > /tmp/secret.txt") == 1 && has("cannot redirect"));
    puts("a pipe cannot be used to read the private log entries either");
    klog(LOG_WARN, "sec", "login-failed uid=-1 subject=victim result=-27"); klog(LOG_INFO, "proc", "visible event marker");
    CHECK(sh("logs -a | grep marker") == 0 && has("visible event marker") && sh("logs -a | grep victim") == 1 && out_len == 0);
    sys_logout();
    ROOT();
    CHECK(sh("logs -a | grep victim | wc -l") == 0 && has("1"));
    puts("protect and lock still stop a redirected pipeline");
    CHECK(sh("protect /tmp/names.txt") == 0 && sh("cat /tmp/text.txt | sort > /tmp/names.txt") == 1 && has("protected") && file_is("/tmp/names.txt", "pear\napple\nfig\napple\nbanana\n"));
    sh("unprotect /tmp/names.txt");
    sys_logout();
}

/* ================= quotes, variables, aliases, scripts ================= */
static void test_shell_features(void) {
    puts("quotes hide pipes");
    ROOT(); make_files();
    CHECK(sh("echo \"a | b\"") == 0 && is("a | b\n"));
    CHECK(sh("echo 'x | y' | cat") == 0 && is("x | y\n") && sh("echo \"p>q\" | cat") == 0 && is("p>q\n"));
    puts("variables and aliases work with pipes");
    CHECK(sh("set F=/tmp/names.txt") == 0 && sh("cat $F | sort | head -n 1") == 0 && is("apple\n"));
    CHECK(sh("alias ll='cat /tmp/names.txt'") == 0 && sh("ll | sort | uniq | wc -l") == 0 && has("4"));
    CHECK(sh("set PAT=apple") == 0 && sh("cat /tmp/names.txt | grep $PAT | wc -l") == 0 && has("2"));
    puts("scripts can contain pipelines");
    CHECK(put("/tmp/s.sh", "cat /tmp/names.txt | sort | head -n 1\ncat /tmp/names.txt | tail -n 1\n") == 0 && sh("source /tmp/s.sh") == 0 && has("apple") && has("banana"));
    puts("repeat feeds a pipe; but a pipeline cannot run inside a captured stage");
    CHECK(sh("repeat 3 echo hi | wc -l") == 0 && has("3"));
    CHECK(sh("source /tmp/s.sh | cat") == 1 && has("cannot run inside another"));
    CHECK(sh("echo done") == 0 && is("done\n"));                                                                  /* the shell is still fine afterwards */
    puts("every stage is logged by NAME, never by arguments");
    klog_init(); klog_set_level(LOG_DEBUG);
    CHECK(sh("cat /tmp/names.txt | grep secretword | wc -l") == 0);
    CHECK(shell_entries("cat ->") == 1 && shell_entries("grep ->") == 1 && shell_entries("wc ->") == 1);
    log_entry_t e; bool leaked = false; for (int i = 0; klog_get((uint32_t)i, true, &e) == 0; i++) if (strstr(e.msg, "secretword") || strstr(e.msg, "names.txt")) leaked = true; CHECK(!leaked);
    sys_logout();
}

/* ================= pager ================= */
static void test_more(void) {
    puts("more as the last stage still pages, because only earlier stages are captured");
    ROOT(); make_files();
    feed("q"); CHECK(sh("cat /tmp/names.txt | sort | more") == 0 && has("apple") && has("pear"));
    sys_logout();
}

static int real_main(void) {
    fresh();
    test_basics();         fresh();
    test_redirect();       fresh();
    test_errors();         fresh();
    test_dash();           fresh();
    test_failures();       fresh();
    test_limits();         fresh();
    test_security();       fresh();
    test_shell_features(); fresh();
    test_more();
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
int main(void) { return run_on_low_stack(real_main); }
