/* Phase 15 tests: control flow (if/elif/else/fi, for, while, break, continue) around command lists and pipelines. Everything runs through
 * the REAL splitter, keyword layer, line executor, pipeline runner, output capture, system calls, MiniFS and the real commands.
 * Only the screen, keyboard, timer and context switch are stubbed. */
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

static void test_if(void) {
    puts("if / then / else / elif / fi");
    ROOT(); make_files();
    CHECK(sh("if true ; then echo yes ; fi") == 0 && is("yes\n"));
    CHECK(sh("if false ; then echo yes ; fi") == 0 && out_len == 0);                         /* no branch taken: status 0 */
    CHECK(sh("if false ; then echo yes ; else echo no ; fi") == 0 && is("no\n"));
    CHECK(sh("if true ; then echo yes ; else echo no ; fi") == 0 && is("yes\n"));
    CHECK(sh("if false ; then echo A ; elif true ; then echo B ; else echo C ; fi") == 0 && is("B\n"));
    CHECK(sh("if false ; then echo A ; elif false ; then echo B ; else echo C ; fi") == 0 && is("C\n"));
    CHECK(sh("if true ; then echo A ; elif true ; then echo B ; fi") == 0 && is("A\n"));        /* only the first true branch runs */
    CHECK(sh("if false ; then echo A ; elif false ; then echo B ; elif true ; then echo C ; fi") == 0 && is("C\n"));
    CHECK(sh("if false ; then echo A ; elif false ; then echo B ; fi") == 0 && out_len == 0);
    puts("the condition is any command list");
    CHECK(sh("if test -f /tmp/names.txt ; then echo exists ; else echo missing ; fi") == 0 && is("exists\n"));
    CHECK(sh("if test -f /tmp/nothing ; then echo exists ; else echo missing ; fi") == 0 && is("missing\n"));
    CHECK(sh("if true && true ; then echo both ; fi") == 0 && is("both\n"));
    CHECK(sh("if true && false ; then echo both ; else echo notboth ; fi") == 0 && is("notboth\n"));
    CHECK(sh("if false || true ; then echo either ; fi") == 0 && is("either\n"));
    CHECK(sh("if cat /tmp/names.txt | grep apple ; then echo found ; fi") == 0 && has("found\n"));
    CHECK(sh("if cat /tmp/names.txt | grep zzz ; then echo found ; else echo nomatch ; fi") == 0 && is("nomatch\n"));
    CHECK(sh("if cat /tmp/missing | sort ; then echo ok ; else echo failed ; fi") == 0 && has("failed\n"));
    CHECK(sh("if test -f /tmp/names.txt ; then echo a ; echo b ; fi") == 0 && is("a\nb\n"));          /* several commands in a body */
    puts("the status of an if is the status of the last command that ran");
    CHECK(sh("if true ; then false ; fi") == 1 && sh("if true ; then true ; fi") == 0 && sh("if false ; then true ; else false ; fi") == 1);
    CHECK(sh("if false ; then echo x ; fi ; echo $?") == 0 && is("0\n"));
    CHECK(sh("if false ; then echo x ; else echo $? ; fi") == 0 && is("1\n"));                         /* $? inside is the condition's status */
    puts("nested ifs and ifs inside lists");
    CHECK(sh("if true ; then if false ; then echo no ; else echo inner ; fi ; fi") == 0 && is("inner\n"));
    CHECK(sh("if true ; then if true ; then echo deep ; fi ; echo out ; fi") == 0 && is("deep\nout\n"));
    CHECK(sh("if true ; then echo A ; fi && echo after") == 0 && is("A\nafter\n"));
    CHECK(sh("if false ; then echo A ; fi || echo notreached") == 0 && out_len == 0);                  /* no branch = status 0, so || is skipped */
    CHECK(sh("false && if true ; then echo X ; fi ; echo end") == 0 && is("end\n"));                   /* a skipped block is skipped as a whole */
    CHECK(sh("true ; if true ; then echo X ; fi ; echo end") == 0 && is("X\nend\n"));
    CHECK(sh("echo first ; if true ; then echo second ; fi ; echo third") == 0 && is("first\nsecond\nthird\n"));
    CHECK(sh("if true ; then echo a > /tmp/if.txt ; echo b >> /tmp/if.txt ; fi") == 0 && file_is("/tmp/if.txt", "a\nb\n"));
    CHECK(sh("if true ; then set V=7 ; fi ; echo $V") == 0 && is("7\n"));
    puts("then / else on their own segment");
    CHECK(sh("if true ; then ; echo yes ; fi") == 0 && is("yes\n") && sh("if false ; then ; echo yes ; else ; echo no ; fi") == 0 && is("no\n"));
    sys_logout();
}

static void test_for(void) {
    puts("for");
    ROOT(); make_files();
    CHECK(sh("for x in a b c ; do echo $x ; done") == 0 && is("a\nb\nc\n"));
    CHECK(sh("for x in 1 ; do echo $x ; done") == 0 && is("1\n"));
    CHECK(sh("for x in ; do echo never ; done") == 0 && out_len == 0);                                 /* no words: no turns, status 0 */
    CHECK(sh("for x in a b ; do echo $x ; echo again ; done") == 0 && is("a\nagain\nb\nagain\n"));
    CHECK(sh("for x in a b c ; do echo $x ; done ; echo $x") == 0 && is("a\nb\nc\nc\n"));                 /* the variable keeps the last value */
    puts("the word list: variables, quotes, several words in one variable");
    CHECK(sh("set L='x y z'") == 0 && sh("for w in $L ; do echo [$w] ; done") == 0 && is("[x]\n[y]\n[z]\n"));
    CHECK(sh("set A=one") == 0 && sh("for w in $A two $A ; do echo $w ; done") == 0 && is("one\ntwo\none\n"));
    CHECK(sh("for w in \"a b\" c ; do echo [$w] ; done") == 0 && is("[a b]\n[c]\n"));
    CHECK(sh("for w in 'x;y' 'p && q' ; do echo $w ; done") == 0 && is("x;y\np && q\n"));
    CHECK(sh("for f in /tmp/names.txt /tmp/nums.csv ; do wc -l $f ; done") == 0 && has("/tmp/names.txt") && has("/tmp/nums.csv"));
    puts("the status of a for is the status of the last turn");
    CHECK(sh("for x in a b ; do false ; done") == 1 && sh("for x in a b ; do true ; done") == 0 && sh("for x in a b ; do test $x = a ; done") == 1);
    puts("for inside lists, pipelines inside for");
    CHECK(sh("for x in 1 2 ; do echo $x | cat ; done") == 0 && is("1\n2\n"));
    CHECK(sh("for x in b a ; do echo $x ; done && echo done") == 0 && is("b\na\ndone\n"));
    CHECK(sh("for x in 1 2 3 ; do echo $x ; done ; echo end") == 0 && is("1\n2\n3\nend\n"));
    CHECK(sh("for n in pear apple ; do cat /tmp/names.txt | grep $n | wc -l ; done") == 0 && has("1") && has("2"));
    CHECK(sh("for x in a b ; do echo $x >> /tmp/for.txt ; done") == 0 && file_is("/tmp/for.txt", "a\nb\n"));
    puts("nested for");
    CHECK(sh("for a in 1 2 ; do for b in x y ; do echo $a$b ; done ; done") == 0 && is("1x\n1y\n2x\n2y\n"));
    puts("errors in the header");
    CHECK(sh("for ; do echo x ; done") == 2 && has("variable name"));
    CHECK(sh("for 1x in a ; do echo x ; done") == 2 && has("variable name") && sh("for a-b in x ; do echo x ; done") == 2);
    CHECK(sh("for x a b ; do echo x ; done") == 2 && has("'in' expected"));
    CHECK(sh("for x in a > f ; do echo x ; done") == 2 && has("'>' and '|'") && sh("for x in a | b ; do echo x ; done") == 2);
    CHECK(sh("for x in 'a ; do echo x ; done") == 2 && has("unterminated"));
    CHECK(sh("for x in a b c d e f g h i j k l m n o p q r s t u v w x y z 1 2 3 ; do echo $x ; done") == 2 && has("at most 28"));
    sys_logout();
}

static void test_while(void) {
    puts("while");
    ROOT(); make_files();
    CHECK(sh("while false ; do echo never ; done") == 0 && out_len == 0);
    CHECK(sh("echo x > /tmp/flag") == 0 && sh("while test -f /tmp/flag ; do echo turn ; rm /tmp/flag ; done") == 0 && is("turn\n"));
    CHECK(sh("while test -f /tmp/flag ; do echo again ; done") == 0 && out_len == 0);
    puts("a counter built from files and variables");
    CHECK(sh("echo 1 > /tmp/c1 ; echo 1 > /tmp/c2 ; echo 1 > /tmp/c3") == 0);
    CHECK(sh("for f in c1 c2 c3 ; do while test -f /tmp/$f ; do echo using-$f ; rm /tmp/$f ; done ; done") == 0 && is("using-c1\nusing-c2\nusing-c3\n"));
    puts("the status of a while is the status of the last turn of its body");
    CHECK(sh("echo x > /tmp/f1") == 0 && sh("while test -f /tmp/f1 ; do rm /tmp/f1 ; false ; done") == 1);
    puts("a runaway loop is stopped");
    CHECK(sh("while true ; do true ; done") == 1 && has("stopped after 1000 turns"));
    CHECK(sh("echo alive") == 0 && is("alive\n"));
    sys_logout();
}

static void test_break_continue(void) {
    puts("break and continue");
    ROOT();
    CHECK(sh("for i in 1 2 3 4 ; do test $i = 3 && break ; echo $i ; done") == 0 && is("1\n2\n"));
    CHECK(sh("for i in 1 2 3 ; do test $i = 2 && continue ; echo $i ; done") == 0 && is("1\n3\n"));
    CHECK(sh("for i in 1 2 3 ; do break ; echo never ; done ; echo after") == 0 && is("after\n"));
    CHECK(sh("for i in 1 2 3 ; do echo $i ; continue ; echo never ; done") == 0 && is("1\n2\n3\n"));
    CHECK(sh("while true ; do echo once ; break ; done") == 0 && is("once\n"));
    puts("inside an if inside a loop");
    CHECK(sh("for i in a b c ; do if test $i = b ; then break ; fi ; echo $i ; done") == 0 && is("a\n"));
    CHECK(sh("for i in a b c ; do if test $i = b ; then continue ; else echo $i ; fi ; done") == 0 && is("a\nc\n"));
    puts("only the innermost loop is affected");
    CHECK(sh("for a in 1 2 ; do for b in x y z ; do test $b = y && break ; echo $a$b ; done ; echo end$a ; done") == 0 && is("1x\nend1\n2x\nend2\n"));
    CHECK(sh("for a in 1 2 ; do for b in x y ; do continue ; done ; echo $a ; done") == 0 && is("1\n2\n"));
    puts("break applies after && and || like any command");
    CHECK(sh("for i in 1 2 3 ; do false || break ; echo $i ; done") == 0 && out_len == 0);
    CHECK(sh("for i in 1 2 3 ; do true || break ; echo $i ; done") == 0 && is("1\n2\n3\n"));
    puts("break outside a loop is an error before anything runs");
    CHECK(sh("echo x > /tmp/side.txt ; break") == 2 && has("only allowed inside a loop") && sh("ls /tmp") == 0 && !has("side.txt"));
    CHECK(sh("continue") == 2 && has("'continue'") && sh("if true ; then break ; fi") == 2);
    sys_logout();
}

static void test_syntax_errors(void) {
    puts("syntax errors: reported first, status 2, nothing runs");
    ROOT();
    CHECK(sh("echo x > /tmp/s1.txt ; if true ; then echo y") == 2 && has("missing 'fi'") && sh("ls /tmp") == 0 && !has("s1.txt"));
    CHECK(sh("echo x > /tmp/s2.txt ; for i in a ; do echo y") == 2 && has("missing 'done'") && sh("ls /tmp") == 0 && !has("s2.txt"));
    CHECK(sh("while true ; do echo y") == 2 && has("missing 'done'"));
    CHECK(sh("echo x > /tmp/s3.txt ; fi") == 2 && has("unexpected 'fi'") && sh("ls /tmp") == 0 && !has("s3.txt"));
    CHECK(sh("done") == 2 && has("unexpected 'done'") && sh("then echo x") == 2 && has("unexpected 'then'") && sh("else echo x") == 2 && sh("elif true") == 2 && sh("do echo x") == 2 && has("unexpected 'do'"));
    CHECK(sh("if true ; echo y ; fi") == 2 && has("'then' expected"));
    CHECK(sh("if ; then echo y ; fi") == 2 && has("empty condition"));
    CHECK(sh("if true ; then ; fi") == 2 && has("empty block after 'then'"));
    CHECK(sh("if true ; then echo a ; else ; fi") == 2 && has("empty block after 'else'"));
    CHECK(sh("while ; do echo y ; done") == 2 && has("empty condition") && sh("while true ; do ; done") == 2 && has("empty block after 'do'") && sh("for i in a ; do ; done") == 2 && has("empty block"));
    CHECK(sh("if true ; then echo a ; else echo b ; else echo c ; fi") == 2 && has("nothing may follow 'else'"));
    CHECK(sh("if true ; then echo a ; else echo b ; elif true ; then echo c ; fi") == 2);
    CHECK(sh("if true ; then echo a ; then echo b ; fi") == 2 && has("then"));
    CHECK(sh("while true ; then echo a ; done") == 2 && sh("while true ; do echo a ; do echo b ; done") == 2);
    CHECK(sh("for i in a ; echo x ; done") == 2 && has("'do' expected"));
    CHECK(sh("for i in a ; do echo x ; fi") == 2 && has("'done' is needed to close a loop") && sh("if true ; then echo x ; done") == 2 && has("'fi' is needed to close an 'if'"));
    CHECK(sh("if true ; then echo x ; fi echo y") == 2 && has("text after") && sh("for i in a ; do echo x ; done | sort") == 2 && has("text after") && sh("for i in a ; do echo x ; done > f") == 2);
    CHECK(sh("if true ; then echo a ; fi ; fi") == 2 && has("unexpected 'fi'"));
    CHECK(sh("if if ; then") == 2);
    puts("nesting: seven levels fit in one line");
    CHECK(sh("if true ; then if true ; then if true ; then if true ; then if true ; then if true ; then if true ; then echo ok ; fi ; fi ; fi ; fi ; fi ; fi ; fi") == 0 && is("ok\n"));
    CHECK(sh("if true ; then if true ; then if true ; then if true ; then if true ; then if true ; then if true ; then if true ; then echo ok ; fi ; fi ; fi ; fi ; fi ; fi ; fi ; fi") == 2 && has("at most 16"));        /* the 16-command limit is reached before the 8-level one */
    sys_logout();
}

static void test_keywords_as_words(void) {
    puts("a keyword is only special as the first word of a command");
    ROOT();
    CHECK(sh("echo if then fi") == 0 && is("if then fi\n") && sh("echo done ; echo for") == 0 && is("done\nfor\n"));
    CHECK(sh("echo 'if' ; echo \"while\"") == 0 && is("if\nwhile\n"));
    CHECK(sh("set W=if") == 0 && sh("echo $W") == 0 && is("if\n"));
    CHECK(sh("echo break ; echo continue") == 0 && is("break\ncontinue\n"));
    CHECK(sh("alias hi='echo HELLO'") == 0 && sh("if true ; then hi ; fi") == 0 && is("HELLO\n") && sh("for i in 1 2 ; do hi ; done") == 0 && is("HELLO\nHELLO\n"));
    sys_logout();
}

static int pressed_after = 0, polls = 0;
static bool fake_ctrl_c(void) { return ++polls > pressed_after; }
static void test_limits_and_interrupt(void) {
    puts("one line may run at most 5000 commands");
    ROOT();
    CHECK(sh("for a in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 ; do for b in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 ; do for c in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 ; do true ; done ; done ; done") == 1 && has("more than 5000 commands"));
    CHECK(sh("echo alive") == 0 && is("alive\n"));
    CHECK(sh("for a in 1 2 3 4 5 6 7 8 9 10 ; do for b in 1 2 3 4 5 6 7 8 9 10 ; do for c in 1 2 3 4 5 6 7 8 9 10 ; do true ; done ; done ; done") == 0);       /* 1000 commands: fine */
    puts("Ctrl+C stops a loop");
    polls = 0; pressed_after = 3; line_set_interrupt_check(fake_ctrl_c);
    CHECK(sh("while true ; do echo x ; done") == 130 && has("^C"));
    int xs = 0; for (const char *p = out; *p; p++) if (*p == 'x') xs++; CHECK(xs == 3);                     /* it ran 3 turns, then stopped */
    polls = 0; pressed_after = 1;
    CHECK(sh("for i in a b c d ; do echo $i ; done") == 130 && has("a\n") && !has("c\n"));
    polls = 0; pressed_after = 1000;
    CHECK(sh("for i in a b ; do echo $i ; done") == 0 && is("a\nb\n"));
    line_set_interrupt_check(NULL);
    CHECK(sh("for i in a b ; do echo $i ; done") == 0 && is("a\nb\n"));
    sys_logout();
}

static void test_script_files(void) {
    puts("source: a block may span several lines");
    ROOT(); make_files();
    CHECK(put("/tmp/m1.sh", "for f in a b c\ndo\n  echo item-$f\ndone\n") == 0 && sh("source /tmp/m1.sh") == 0 && is("item-a\nitem-b\nitem-c\n"));
    CHECK(put("/tmp/m2.sh", "# a comment\nif test -f /tmp/names.txt\nthen\n  echo has-names\nelse\n  echo no-names\nfi\necho after\n") == 0 && sh("source /tmp/m2.sh") == 0 && is("has-names\nafter\n"));
    CHECK(put("/tmp/m3.sh", "for a in 1 2\ndo\n  if test $a = 2\n  then\n    echo two\n  fi\n  echo seen-$a\ndone\n") == 0 && sh("source /tmp/m3.sh") == 0 && is("seen-1\ntwo\nseen-2\n"));
    CHECK(put("/tmp/m4.sh", "for f in a b c\ndo\n  # comment inside a block\n\n  echo $f\ndone\n") == 0 && sh("source /tmp/m4.sh") == 0 && is("a\nb\nc\n"));
    CHECK(put("/tmp/m5.sh", "echo start\nwhile test -f /tmp/names.txt\ndo\n  echo loop\n  rm /tmp/names.txt\ndone\necho end\n") == 0 && sh("source /tmp/m5.sh") == 0 && is("start\nloop\nend\n"));
    CHECK(put("/tmp/m6.sh", "if true ; then echo one-line ; fi\nfor i in x y ; do echo $i ; done\n") == 0 && sh("source /tmp/m6.sh") == 0 && is("one-line\nx\ny\n"));
    puts("a script whose block never ends reports it and runs nothing from that block");
    CHECK(put("/tmp/m7.sh", "echo before\nif true\nthen\n  echo inside\n") == 0 && sh("source /tmp/m7.sh") == 2 && has("before\n") && has("unfinished if/for/while that began on line 2") && !has("inside\n"));
    CHECK(put("/tmp/m8.sh", "echo x > /tmp/m8.out\nfi\necho y\n") == 0 && sh("source /tmp/m8.sh") == 0 && has("unexpected 'fi'") && has("y\n"));
    CHECK(put("/tmp/m9.sh", "for i in 1 2\ndo\n  break\n  echo never\ndone\necho ok\n") == 0 && sh("source /tmp/m9.sh") == 0 && is("ok\n"));
    puts("Windows line endings");
    CHECK(put("/tmp/crlf.sh", "for f in a b\r\ndo\r\n  echo $f\r\ndone\r\n") == 0 && sh("source /tmp/crlf.sh") == 0 && is("a\nb\n"));
    puts("a script can be piped, and a block inside source needs no capture of its own");
    CHECK(sh("source /tmp/m1.sh | wc -l") == 0 && has("3"));
    sys_logout();
}

static void test_logging_and_memory(void) {
    puts("only commands that really ran are logged, keywords never are");
    ROOT(); klog_init(); klog_set_level(LOG_DEBUG);
    CHECK(sh("for i in a b ; do echo $i ; done ; if false ; then echo skipped ; fi") == 0);
    CHECK(shell_entries("echo ->") == 2 && shell_entries("for") == 0 && shell_entries("if") == 0 && shell_entries("false ->") == 1);
    log_entry_t e; bool leaked = false; for (int i = 0; klog_get((uint32_t)i, true, &e) == 0; i++) if (strstr(e.msg, "skipped")) leaked = true; CHECK(!leaked);
    puts("many blocks in a row neither leak memory nor process allocations");
    memory_summary_t m0, m1; memory_summary(&m0);
    for (int i = 0; i < 120; i++) { if (sh("for x in a b c ; do if test $x = b ; then continue ; fi ; echo $x ; done") != 0) { CHECK(false); break; } }
    for (int i = 0; i < 80; i++) { if (sh("if true ; then echo x ; else") != 2) { CHECK(false); break; } }
    for (int i = 0; i < 40; i++) { if (sh("while true ; do true ; done") != 1) { CHECK(false); break; } }
    memory_summary(&m1); CHECK(m1.free_bytes == m0.free_bytes);
    sys_logout();
}

static void test_permissions(void) {
    puts("control flow changes no permissions");
    ROOT(); put("/tmp/secret.txt", "TOP SECRET\n"); sh("chmod 600 /tmp/secret.txt"); sys_logout();
    login_as("student", "student123");
    CHECK(sh("for f in /tmp/secret.txt ; do cat $f ; done") == 1 && has("ermission denied") && !has("TOP SECRET"));
    CHECK(sh("if cat /tmp/secret.txt ; then echo leaked ; else echo denied ; fi") == 0 && has("denied\n") && !has("TOP SECRET") && !has("leaked"));
    CHECK(sh("for x in 1 ; do echo hacked > /etc/passwd ; done") == 1 && has("cannot redirect"));
    CHECK(sh("for USER in x ; do echo $USER ; done") == 0 && is("x\n"));                         /* $USER is an ordinary variable: the prompt and the permissions use the real credentials */
    CHECK(sh("whoami") == 0 && has("student") && !has("x\n"));
    CHECK(sh("while true ; do sudo ls ; break ; done") == 0 && !has("cannot be part of a pipeline"));      /* sudo is fine in a loop; with no password typed it is simply refused */
    CHECK(sh("id") == 0 && has("student"));
    sys_logout();
}

static int real_main(void) {
    fresh();
    test_if();                    fresh();
    test_for();                   fresh();
    test_while();                 fresh();
    test_break_continue();        fresh();
    test_syntax_errors();         fresh();
    test_keywords_as_words();     fresh();
    test_limits_and_interrupt();  fresh();
    test_script_files();          fresh();
    test_logging_and_memory();    fresh();
    test_permissions();
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
int main(void) { return run_on_low_stack(real_main); }
