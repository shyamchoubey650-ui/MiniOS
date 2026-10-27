/* Phase 16 tests: Tab completion and the prompt path against the REAL command table, shell variables, MiniFS and system calls
 * (shell/compenv.c). The pure parts (edit buffer, scrollback ring, completion logic) are tested in tests/test_lineedit.c. */
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
#include "include/complete.h"
#include "include/lineedit.h"
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



static comp_result_t R;
static comp_kind_t comp(const char *line) { return complete_word(line, strlen(line), complete_env_system(), &R); }

static void test_commands_and_keywords(void) {
    puts("commands come from the real command table");
    ROOT();
    CHECK(comp("ech") == COMP_UNIQUE && !strcmp(R.text, "echo") && R.append_space);
    CHECK(comp("logt") == COMP_UNIQUE && !strcmp(R.text, "logtail"));
    CHECK(comp("log") == COMP_LIST && R.ncand >= 9);                                              /* logs logsave logtail logfind logstats logclear loglevel login logout ... */
    CHECK(comp("loglev") == COMP_UNIQUE && !strcmp(R.text, "loglevel"));
    CHECK(comp("zzzz") == COMP_NONE);
    bool has_sort = false; comp("so"); for (size_t i = 0; i < R.ncand; i++) if (!strcmp(R.cand[i], "sort")) has_sort = true; CHECK(has_sort || comp("sor") == COMP_UNIQUE);
    CHECK(comp("sor") == COMP_UNIQUE && !strcmp(R.text, "sort"));
    CHECK(comp("th") == COMP_UNIQUE && !strcmp(R.text, "then") && comp("don") == COMP_UNIQUE && !strcmp(R.text, "done") && comp("whil") == COMP_UNIQUE && !strcmp(R.text, "while") && comp("whi") == COMP_LIST);
    CHECK(comp("echo a | sor") == COMP_UNIQUE && !strcmp(R.text, "sort") && comp("true && ech") == COMP_UNIQUE && comp("sudo logt") == COMP_UNIQUE);
    comp(""); CHECK(R.total >= 300 && R.ncand == COMP_MAX);                                         /* every command, shown capped */
    sys_logout();
}

static void test_files(void) {
    puts("file names come from MiniFS through the system calls");
    ROOT();
    CHECK(comp("ls /et") == COMP_UNIQUE && !strcmp(R.text, "/etc/") && !R.append_space);
    CHECK(comp("cat /etc/pas") == COMP_UNIQUE && !strcmp(R.text, "/etc/passwd") && R.append_space);
    CHECK(comp("cat /etc/") == COMP_LIST && R.ncand >= 3);
    CHECK(sh("cd /etc") == 0 && comp("cat ho") == COMP_UNIQUE && !strcmp(R.text, "hostname"));                 /* relative to the working directory */
    CHECK(sh("cd /tmp") == 0 && put("/tmp/alpha.txt", "a") == 0 && put("/tmp/alpine.txt", "a") == 0 && sh("mkdir /tmp/work") == 0);
    CHECK(comp("cat alp") == COMP_LIST && R.ncand == 2 && !strcmp(R.cand[0], "alpha.txt") && !strcmp(R.cand[1], "alpine.txt"));
    CHECK(comp("cat alph") == COMP_UNIQUE && !strcmp(R.text, "alpha.txt"));
    CHECK(comp("cd wo") == COMP_UNIQUE && !strcmp(R.text, "work/") && !R.append_space);
    CHECK(comp("cat /tmp/alpi") == COMP_UNIQUE && !strcmp(R.text, "/tmp/alpine.txt"));
    bool dir_flag = false; comp("ls /"); for (size_t i = 0; i < R.ncand; i++) if (!strcmp(R.cand[i], "etc")) dir_flag = R.cand_dir[i]; CHECK(dir_flag);
    CHECK(comp("cat nothing") == COMP_NONE && comp("cat /nodir/x") == COMP_NONE);
    puts("hidden files, names with spaces, special characters");
    CHECK(put("/tmp/.dotfile", "x") == 0 && put("/tmp/my file", "x") == 0 && put("/tmp/semi;colon", "x") == 0);
    CHECK(comp("cat /tmp/.dot") == COMP_UNIQUE && !strcmp(R.text, "/tmp/.dotfile"));
    comp("cat /tmp/"); bool bad = false; for (size_t i = 0; i < R.ncand; i++) if (R.cand[i][0] == '.' || strchr(R.cand[i], ' ') || strchr(R.cand[i], ';')) bad = true; CHECK(!bad);
    CHECK(comp("cat /tmp/my") == COMP_NONE && comp("cat /tmp/semi") == COMP_NONE);
    puts("a big directory is capped and counted");
    CHECK(sh("mkdir /tmp/big") == 0);
    for (int i = 0; i < 70; i++) { char path[40]; snprintf(path, sizeof path, "/tmp/big/file%02d", i); put(path, "x"); }
    CHECK(comp("ls /tmp/big/f") == COMP_PARTIAL && !strcmp(R.text, "/tmp/big/file"));
    CHECK(comp("ls /tmp/big/file") == COMP_LIST && R.ncand == COMP_MAX && R.total == 70);
    CHECK(comp("ls /tmp/big/file69") == COMP_UNIQUE && !strcmp(R.text, "/tmp/big/file69"));
    puts("redirection targets");
    CHECK(comp("echo x > /tmp/alph") == COMP_UNIQUE && !strcmp(R.text, "/tmp/alpha.txt"));
    sys_logout();
}

static void test_variables(void) {
    puts("variables come from the shell's variable table");
    ROOT();
    CHECK(sh("set MYVARIABLE=1") == 0 && sh("set MYOTHER=2") == 0);
    CHECK(comp("echo $MYV") == COMP_UNIQUE && !strcmp(R.text, "$MYVARIABLE") && !R.append_space);
    CHECK(comp("echo $MY") == COMP_LIST && R.ncand == 2 && !strcmp(R.cand[0], "MYOTHER") && !strcmp(R.cand[1], "MYVARIABLE"));
    CHECK(comp("echo $MYO") == COMP_UNIQUE && !strcmp(R.text, "$MYOTHER"));
    CHECK(comp("echo ${MYV") == COMP_UNIQUE && !strcmp(R.text, "${MYVARIABLE}"));
    CHECK(comp("echo $NOSUCHVAR") == COMP_NONE);
    CHECK(sh("unset MYVARIABLE") == 0 && comp("echo $MYV") == COMP_NONE);
    sys_logout();
}

static void test_permissions(void) {
    puts("completion shows only what the user may list");
    ROOT();
    CHECK(sh("mkdir /tmp/vault") == 0 && put("/tmp/vault/treasure.txt", "gold") == 0 && sh("chmod 700 /tmp/vault") == 0);
    CHECK(comp("ls /tmp/vault/tre") == COMP_UNIQUE && !strcmp(R.text, "/tmp/vault/treasure.txt"));
    sys_logout();
    login_as("student", "student123");
    CHECK(comp("ls /tmp/vault/tre") == COMP_NONE && comp("ls /tmp/vault/") == COMP_NONE);                 /* not even the names leak */
    CHECK(comp("ls /tmp/va") == COMP_UNIQUE && !strcmp(R.text, "/tmp/vault/"));                              /* the directory itself is visible in /tmp */
    sys_logout();
}

static void test_prompt_path_real(void) {
    puts("the prompt path uses the real home directory");
    login_as("admin", "admin123");
    user_info_t ui; char cwd[FS_PATH_MAX], shown[64];
    CHECK(sys_user_lookup("admin", -1, &ui) == 0 && !strcmp(ui.home, "/home/admin"));
    CHECK(sys_chdir(ui.home) == 0);                                                                        /* what the login screen does */
    CHECK(sys_getcwd(cwd, sizeof cwd) >= 0 && prompt_path(cwd, ui.home, shown, sizeof shown) > 0 && !strcmp(shown, "~"));            /* logging in puts you in your home */
    CHECK(sh("mkdir docs") == 0 && sh("cd docs") == 0 && sys_getcwd(cwd, sizeof cwd) >= 0 && (prompt_path(cwd, ui.home, shown, sizeof shown), !strcmp(shown, "~/docs")));
    CHECK(sh("cd /tmp") == 0 && sys_getcwd(cwd, sizeof cwd) >= 0 && (prompt_path(cwd, ui.home, shown, sizeof shown), !strcmp(shown, "/tmp")));
    CHECK(sh("cd /") == 0 && sys_getcwd(cwd, sizeof cwd) >= 0 && (prompt_path(cwd, ui.home, shown, sizeof shown), !strcmp(shown, "/")));
    sys_logout();
    login_as("root", "root123");
    CHECK(sys_user_lookup("root", -1, &ui) == 0 && sys_chdir(ui.home) == 0 && sys_getcwd(cwd, sizeof cwd) >= 0 && (prompt_path(cwd, ui.home, shown, sizeof shown), !strcmp(shown, "~")));
    sys_logout();
}

static void test_kbdinfo(void) {
    puts("kbdinfo tells you what the keyboard sent (host: no keys, so it says so)");
    login_as("student", "student123");
    CHECK(sh("kbdinfo") == 0 && has("Last scancodes") && has("none yet") && has("Last keys") && has("E0 49") && has("Mouse") && has("none found") && has("mouse wheel scroll"));
    sys_logout();
}

static int real_main(void) {
    fresh();
    test_commands_and_keywords(); fresh();
    test_files();                 fresh();
    test_variables();             fresh();
    test_permissions();           fresh();
    test_prompt_path_real(); fresh();
    test_kbdinfo();
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
int main(void) { return run_on_low_stack(real_main); }
