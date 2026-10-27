/* Phase 14 tests: command lists (a ; b, a && b, a || b) around pipelines. Everything runs through the REAL splitter, line executor,
 * pipeline runner, output capture, system calls, MiniFS and the real commands. Only the screen, keyboard, timer and context switch are stubbed. */
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
#include "include/lineexec.h"
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
static int sh(const char *raw) {                     /* same path as shell/shell.c: line_execute does lists, aliases, variables, pipelines */
    bool ran;
    out_reset();
    int st = line_execute(raw, lookup, owner_yes, &ran);
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
static int shell_entries(const char *prefix) {
    int n = 0; log_entry_t e;
    for (int i = 0; klog_get((uint32_t)i, true, &e) == 0; i++) if (!strcmp(e.src, "shell") && !strncmp(e.msg, prefix, strlen(prefix))) n++;
    return n;
}
static void make_files(void) {
    put("/tmp/names.txt", "pear\napple\nfig\napple\nbanana\n");
    put("/tmp/nums.csv", "ada,90\nlinus,85\ngrace,95\n");
}

static void test_semicolon(void) {
    puts("';' runs the next command whatever happened");
    ROOT(); make_files();
    CHECK(sh("echo a ; echo b") == 0 && is("a\nb\n"));
    CHECK(sh("echo a;echo b;echo c") == 0 && is("a\nb\nc\n"));
    CHECK(sh("echo a ;") == 0 && is("a\n") && sh("echo a ;   ") == 0 && is("a\n"));
    CHECK(sh("false ; echo after") == 0 && is("after\n"));
    CHECK(sh("true ; false") == 1 && sh("false ; true") == 0);
    CHECK(sh("echo one > /tmp/l.txt ; echo two >> /tmp/l.txt ; cat /tmp/l.txt") == 0 && is("one\ntwo\n") && file_is("/tmp/l.txt", "one\ntwo\n"));
    CHECK(sh("cd /tmp ; pwd") == 0 && is("/tmp\n") && sh("pwd") == 0 && is("/tmp\n"));
    CHECK(sh("echo a ; nosuchcmd ; echo c") == 0 && has("a\n") && has("unknown command 'nosuchcmd'") && has("c\n"));
    CHECK(sh("echo 1;echo 2;echo 3;echo 4;echo 5;echo 6;echo 7;echo 8") == 0 && is("1\n2\n3\n4\n5\n6\n7\n8\n"));
    sys_logout();
}

static void test_and_or(void) {
    puts("'&&' runs the next command only after success, '||' only after failure");
    ROOT(); make_files();
    CHECK(sh("true && echo yes") == 0 && is("yes\n"));
    CHECK(sh("false && echo yes") == 1 && out_len == 0);
    CHECK(sh("false || echo no") == 0 && is("no\n"));
    CHECK(sh("true || echo no") == 0 && out_len == 0);
    puts("left to right with equal priority");
    CHECK(sh("true && echo A || echo B") == 0 && is("A\n"));
    CHECK(sh("false && echo A || echo B") == 0 && is("B\n"));
    CHECK(sh("true || echo A && echo B") == 0 && is("B\n"));                          /* true; skip A (status stays 0); B runs */
    CHECK(sh("false || false || echo third") == 0 && is("third\n"));
    CHECK(sh("true && true && echo all") == 0 && is("all\n") && sh("true && false && echo never") == 1 && out_len == 0);
    CHECK(sh("false && echo x && echo y") == 1 && out_len == 0);                       /* skipped commands keep the status */
    CHECK(sh("echo a && false") == 1 && sh("false || true") == 0);
    puts("test, the classic use");
    CHECK(sh("test -f /tmp/names.txt && echo exists || echo missing") == 0 && is("exists\n"));
    CHECK(sh("test -f /tmp/nothing && echo exists || echo missing") == 0 && is("missing\n"));
    CHECK(sh("test -d /tmp && echo dir") == 0 && is("dir\n") && sh("test -d /tmp/names.txt || echo notdir") == 0 && is("notdir\n"));
    puts("unknown commands fail with 127 and so decide the next step");
    CHECK(sh("nosuchcmd && echo ran") == 127 && has("unknown command") && !has("ran\n"));
    CHECK(sh("nosuchcmd || echo recovered") == 0 && has("recovered"));
    puts("$? sees the previous command");
    CHECK(sh("false ; echo $?") == 0 && is("1\n") && sh("true ; echo $?") == 0 && is("0\n"));
    CHECK(sh("false && echo x ; echo $?") == 0 && is("1\n"));
    CHECK(sh("nosuchcmd ; echo $?") == 0 && has("127"));
    sys_logout();
}

static void test_with_pipes(void) {
    puts("a pipeline is one command of a list");
    ROOT(); make_files();
    CHECK(sh("cat /tmp/names.txt | sort | head -n 1 ; echo done") == 0 && is("apple\ndone\n"));
    CHECK(sh("echo a | cat && echo b") == 0 && is("a\nb\n"));
    CHECK(sh("cat /tmp/missing | sort || echo fallback") == 0 && has("Error:") && has("pipeline stopped") && has("fallback\n"));
    CHECK(sh("cat /tmp/names.txt | grep zzz || echo nomatch") == 0 && is("nomatch\n"));
    CHECK(sh("cat /tmp/names.txt | grep apple && echo found") == 0 && is("apple\napple\nfound\n"));
    CHECK(sh("cat /tmp/names.txt | sort > /tmp/s.txt && cat /tmp/s.txt | head -n 2") == 0 && is("apple\napple\n"));
    CHECK(sh("cat /tmp/nums.csv | cut -d , -f 2 | sum ; cat /tmp/nums.csv | cut -d , -f 2 | max") == 0 && is("270\n95\n"));
    CHECK(sh("cat /tmp/names.txt | nosuch ; echo still") == 0 && has("unknown command") && has("still\n"));
    CHECK(sh("a | b | c | d | e ; echo ok") == 0 && has("at most 4") && has("ok\n"));
    puts("wc prints no name for a pipe, so its number can feed the next command");
    CHECK(sh("cat /tmp/names.txt | wc -l | sum") == 0 && is("5\n"));
    CHECK(sh("cat /tmp/names.txt | wc -l") == 0 && has("5") && !has("-"));
    CHECK(sh("wc -l /tmp/names.txt") == 0 && has("/tmp/names.txt"));
    sys_logout();
}

static void test_quotes_vars(void) {
    puts("quotes hide the operators");
    ROOT(); make_files();
    CHECK(sh("echo \"a;b\" ; echo 'c && d'") == 0 && is("a;b\nc && d\n"));
    CHECK(sh("echo \"x || y\"") == 0 && is("x || y\n") && sh("echo 'p & q'") == 0 && is("p & q\n"));
    puts("variables and aliases are expanded per command, just before it runs");
    CHECK(sh("set X=1 ; echo $X") == 0 && is("1\n"));
    CHECK(sh("set A=1 ; set B=$A$A ; echo $B") == 0 && is("11\n"));
    CHECK(sh("set P=/tmp/names.txt && cat $P | wc -l") == 0 && has("5"));
    CHECK(sh("alias hi='echo HELLO'") == 0 && sh("true ; hi") == 0 && is("HELLO\n") && sh("hi ; hi") == 0 && is("HELLO\nHELLO\n"));
    CHECK(sh("unset X ; echo [$X]") == 0 && is("[]\n"));
    sys_logout();
}

static void test_errors(void) {
    puts("list syntax errors: nothing runs, status 2");
    ROOT();
    CHECK(sh("echo x > /tmp/side.txt ;; echo y") == 2 && has("missing command next to") && sh("ls /tmp") == 0 && !has("side.txt"));
    CHECK(sh(";") == 2 && has("missing command") && sh("&& echo x") == 2 && sh("echo x &&") == 2 && sh("|| echo x") == 2 && sh("echo x ||") == 2);
    CHECK(sh("echo x > /tmp/side.txt & echo y") == 2 && has("background jobs") && has("spawn") && sh("ls /tmp") == 0 && !has("side.txt"));
    CHECK(sh("echo a ; echo \"b") == 2 && has("unterminated") && !has("a\n"));                                  /* the quote is found before anything runs */
    CHECK(sh("1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17") == 2 && has("at most 16"));
    puts("an error inside ONE command does not stop the earlier ones, but && and || see the failure");
    CHECK(sh("echo before ; echo hi > ; echo after") == 0 && has("before\n") && has("missing file name") && has("after\n"));
    CHECK(sh("echo hi > && echo never") == 2 && !has("never"));
    CHECK(sh("echo a | | cat ; echo b") == 0 && has("missing command next to '|'") && has("b\n"));
    CHECK(sh("echo a | | cat || echo recovered") == 0 && has("recovered"));
    sys_logout();
}

static void test_scripts_and_logs(void) {
    puts("scripts, repeat and pipes can carry lists");
    ROOT(); make_files();
    CHECK(put("/tmp/s.sh", "echo s1 ; echo s2\ntest -f /tmp/names.txt && echo seen\n") == 0 && sh("source /tmp/s.sh") == 0 && is("s1\ns2\nseen\n"));
    CHECK(sh("source /tmp/s.sh | wc -l") == 0 && has("3"));                                     /* a list needs no capture of its own: it can run inside a captured stage */
    CHECK(sh("repeat 2 echo hi ; echo end") == 0 && is("hi\nhi\nend\n"));
    CHECK(sh("repeat 3 echo hi | wc -l ; echo end") == 0 && has("3") && has("end"));
    puts("only commands that really ran are logged");
    klog_init(); klog_set_level(LOG_DEBUG);
    CHECK(sh("false && echo skipped ; true || echo skipped2 ; echo ran") == 0 && is("ran\n"));
    CHECK(shell_entries("false ->") == 1 && shell_entries("true ->") == 1 && shell_entries("echo ->") == 1);       /* one echo ran, two were skipped */
    log_entry_t e; bool leaked = false; for (int i = 0; klog_get((uint32_t)i, true, &e) == 0; i++) if (strstr(e.msg, "skipped")) leaked = true; CHECK(!leaked);
    puts("sudo is fine in a list: only pipelines forbid it");
    feed(""); CHECK(sh("sudo ls ; echo x") == 0 && has("x\n"));                                 /* sudo fails without a password, the list goes on */
    CHECK(!has("cannot be part of a pipeline"));
    puts("many lists in a row neither leak memory nor process allocations");
    memory_summary_t m0, m1; memory_summary(&m0);
    for (int i = 0; i < 150; i++) { if (sh("cat /tmp/names.txt | sort | uniq | wc -l && echo ok ; false || true") != 0) { CHECK(false); break; } }
    for (int i = 0; i < 80; i++) { if (sh("cat /tmp/missing | sort ; echo x && nosuch") != 127) { CHECK(false); break; } }
    memory_summary(&m1); CHECK(m1.free_bytes == m0.free_bytes);
    sys_logout();
}

static void test_permissions(void) {
    puts("lists do not change permissions: every command still runs as the logged-in user");
    ROOT(); put("/tmp/secret.txt", "TOP SECRET\n"); sh("chmod 600 /tmp/secret.txt"); sys_logout();
    login_as("student", "student123");
    CHECK(sh("cat /tmp/secret.txt ; echo next") == 0 && has("ermission denied") && !has("TOP SECRET") && has("next\n"));
    CHECK(sh("cat /tmp/secret.txt && echo leaked") == 1 && !has("TOP SECRET") && !has("leaked"));
    CHECK(sh("cat /tmp/secret.txt || echo denied-as-expected") == 0 && has("denied-as-expected"));
    CHECK(sh("echo x > /etc/passwd ; echo done") == 0 && has("cannot redirect") && has("done\n"));
    CHECK(sh("logs -a | grep victim ; echo checked") == 0 && has("checked\n"));
    sys_logout();
}

static int real_main(void) {
    fresh();
    test_semicolon();       fresh();
    test_and_or();          fresh();
    test_with_pipes();      fresh();
    test_quotes_vars();     fresh();
    test_errors();          fresh();
    test_scripts_and_logs(); fresh();
    test_permissions();
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
int main(void) { return run_on_low_stack(real_main); }
