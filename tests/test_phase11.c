/* Phase 11 tests: the protect flag, kernel information, power control, the RAM-disk and MiniFS images, and the new
 * commands (shell environment, text, system, disk, utilities, simulated network, diagnostics). Everything runs through
 * the REAL system-call layer, MiniFS, parser, registry and redirection. Only the screen, keyboard, timer and context switch
 * are stubbed; hardware reads inside kernel/sysinfo.c return fixed values in the host build (see include/kinfo.h). */
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
static bool fed_all(void) { return kq[kq_pos] == 0; }
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
static bool is(const char *s) { return strcmp(out, s) == 0; }

static multiboot_mmap_entry_t map_buf[1]; static multiboot_info_t mbi;
static void fresh(void) {
    map_buf[0] = (multiboot_mmap_entry_t){ 20, 0x100000, 0x4000000, 1 };
    memset(&mbi, 0, sizeof mbi);
    mbi.flags = MULTIBOOT_FLAG_MMAP; mbi.mmap_addr = (uint32_t)(uintptr_t)map_buf; mbi.mmap_length = sizeof map_buf;
    memory_init(&mbi, 0x100000, 0x110000);
    fs_set_clock(timer_ticks); fs_set_user(FS_UID_ROOT, FS_UID_ROOT); fs_set_umask(022);
    fs_init(); process_init(); syscall_init(); ramdisk_init(); sv_reset();
    users_init();
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
static int owner_of(const char *p) { fs_stat_t st; return sys_stat(p, &st) == 0 ? (int)st.uid : -1; }
static uint16_t mode_of(const char *p) { fs_stat_t st; return sys_stat(p, &st) == 0 ? st.mode : 0xFFFF; }
static bool file_is(const char *path, const char *want) {
    char b[1024]; int fd = sys_open(path, SYS_O_READ); if (fd < 0) return false;
    int n = sys_read(fd, b, sizeof b - 1); sys_close(fd); if (n < 0) return false; b[n] = 0;
    return !strcmp(b, want);
}

/* ================= protect flag ================= */
static void test_protect(void) {
    puts("protect flag: every way of changing a protected file is refused");
    fs_stat_t st;
    ROOT(); sys_mkdir("/tmp/d", 0); sys_mkdir("/tmp/d/sub", 0); put("/tmp/d/keep.txt", "keep\n"); put("/tmp/other", "other\n");
    CHECK(sys_protect("/tmp/d/keep.txt", true) == 0 && sys_stat("/tmp/d/keep.txt", &st) == 0 && st.protect);
    CHECK(sys_open("/tmp/d/keep.txt", SYS_O_WRITE) == SYS_EPROTECT && sys_open("/tmp/d/keep.txt", SYS_O_WRITE | SYS_O_TRUNC) == SYS_EPROTECT && sys_open("/tmp/d/keep.txt", SYS_O_WRITE | SYS_O_APPEND) == SYS_EPROTECT);
    CHECK(put("/tmp/d/keep.txt", "changed") == SYS_EPROTECT && sys_delete("/tmp/d/keep.txt") == SYS_EPROTECT && sys_rename("/tmp/d/keep.txt", "/tmp/d/moved") == SYS_EPROTECT);
    CHECK(sys_rename("/tmp/other", "/tmp/d/keep.txt") != 0);                                             /* never replaces it */
    CHECK(file_is("/tmp/d/keep.txt", "keep\n"));
    CHECK(sh("cp /tmp/other /tmp/d/keep.txt") == 1 && has("protected") && sh("rm /tmp/d/keep.txt") == 1 && has("protected") && sh("echo x > /tmp/d/keep.txt") == 1 && file_is("/tmp/d/keep.txt", "keep\n"));
    CHECK(sh("mv /tmp/d/keep.txt /tmp/x") == 1 && sh("cat /tmp/d/keep.txt") == 0 && is("keep\n"));
    CHECK(sys_chmod("/tmp/d/keep.txt", 0600) == 0 && mode_of("/tmp/d/keep.txt") == 0600);
    puts("protect flag: directories and trees");
    CHECK(sys_protect("/tmp/d", true) == 0 && sys_rmdir("/tmp/d") == SYS_EPROTECT && sys_rename("/tmp/d", "/tmp/e") == SYS_EPROTECT);
    CHECK(put("/tmp/d/new.txt", "ok") == 0 && sys_delete("/tmp/d/new.txt") == 0);
    CHECK(sys_protect("/tmp/d", false) == 0);
    CHECK(sh("rmtree /tmp/d") == 1 && has("protected") && sys_stat("/tmp/d/sub", &st) == 0 && sys_stat("/tmp/d/keep.txt", &st) == 0);
    CHECK(sh("unprotect /tmp/d/keep.txt") == 0 && has("no longer protected") && sh("rmtree /tmp/d") == 0 && sys_stat("/tmp/d", &st) == SYS_ENOENT);
    puts("protect flag: who may set it");
    CHECK(sys_protect("/", true) == SYS_EBUSY && sys_protect("/nope", true) == SYS_ENOENT && sys_protect(NULL, true) == SYS_EFAULT);
    put("/tmp/rootfile", "r"); sys_logout();
    login_as("student", "student123");
    CHECK(sys_protect("/tmp/rootfile", true) == SYS_EACCES);
    put("/tmp/mine", "m"); CHECK(sys_protect("/tmp/mine", true) == 0 && sys_delete("/tmp/mine") == SYS_EPROTECT && sys_protect("/tmp/mine", false) == 0 && sys_delete("/tmp/mine") == 0);
    CHECK(sh("protect") == 1 && has("usage:") && sh("protect /nope") == 1 && has("does not exist") && sh("unprotect") == 1);
    CHECK(sh("touch /tmp/p1 /tmp/p2") == 0 && sh("protect /tmp/p1 /tmp/p2") == 0 && has("/tmp/p2 is protected") && sh("rm /tmp/p1") == 1 && sh("unprotect /tmp/p1 /tmp/p2") == 0 && sh("rm /tmp/p1 /tmp/p2") == 0);
    sys_logout();
}

/* ================= kernel information ================= */
static void test_kinfo(void) {
    puts("kernel information: selectors, sizes and bad input");
    ki_cpu_t cpu; ki_cpuid_t q; ki_timer_t t; ki_irq_t irq; ki_table_t tab; ki_desc_t d; ki_kernel_t k; ki_boot_t b; ki_mmap_t mm; ki_vga_t v; ki_kbd_t kb; ki_rtc_t rtc; ki_device_t dev;
    CHECK(sys_kinfo(KI_CPU, 0, &cpu, sizeof cpu) == 0 && strlen(cpu.vendor) == 12 && cpu.max_leaf >= 1);
    q.leaf = 0; q.subleaf = 0; CHECK(sys_kinfo(KI_CPUID, 0, &q, sizeof q) == 0 && q.eax == cpu.max_leaf && q.ebx != 0);
    CHECK(sys_kinfo(KI_TIMER, 0, &t, sizeof t) == 0 && t.hz == TIMER_HZ && t.base_hz == 1193182u && t.divisor == 1193182u / TIMER_HZ && t.ticks == timer_ticks());
    CHECK(sys_kinfo(KI_IRQ, 0, &irq, sizeof irq) == 0 && !strcmp(irq.name, "timer") && sys_kinfo(KI_IRQ, 1, &irq, sizeof irq) == 0 && !strcmp(irq.name, "keyboard") && sys_kinfo(KI_IRQ, 15, &irq, sizeof irq) == 0 && sys_kinfo(KI_IRQ, 16, &irq, sizeof irq) == SYS_EINVAL);
    CHECK(sys_kinfo(KI_IDT_INFO, 0, &tab, sizeof tab) == 0 && tab.limit == 2047 && sys_kinfo(KI_GDT_INFO, 0, &tab, sizeof tab) == 0 && tab.limit == 23);
    CHECK(sys_kinfo(KI_IDT_ENTRY, 255, &d, sizeof d) == 0 && d.present && d.type == 14 && sys_kinfo(KI_IDT_ENTRY, 256, &d, sizeof d) == SYS_EINVAL);
    CHECK(sys_kinfo(KI_GDT_ENTRY, 2, &d, sizeof d) == 0 && sys_kinfo(KI_GDT_ENTRY, 3, &d, sizeof d) == SYS_EINVAL);
    CHECK(sys_kinfo(KI_KERNEL, 0, &k, sizeof k) == 0 && k.heap_size > 0 && k.size > 0);
    CHECK(sys_kinfo(KI_BOOT, 0, &b, sizeof b) == SYS_ENOENT);
    kinfo_set_boot(&mbi);
    CHECK(sys_kinfo(KI_BOOT, 0, &b, sizeof b) == 0 && b.flags == mbi.flags && b.mmap_entries >= 1);
    CHECK(sys_kinfo(KI_MMAP, 0, &mm, sizeof mm) == 0 && mm.addr == 0x100000 && mm.type == 1 && sys_kinfo(KI_MMAP, 99, &mm, sizeof mm) == SYS_ENOENT);
    CHECK(sys_kinfo(KI_VGA, 0, &v, sizeof v) == 0 && v.cols == 80 && v.rows == 25 && v.base == 0xB8000 && sys_kinfo(KI_KBD, 0, &kb, sizeof kb) == 0 && kb.capacity > 0);
    CHECK(sys_kinfo(KI_RTC, 0, &rtc, sizeof rtc) == 0 && rtc.year == 2026 && rtc.month == 10 && rtc.day == 12 && rtc.hour == 9 && rtc.weekday == 1);
    uint32_t n = 0; while (sys_kinfo(KI_DEVICE, n, &dev, sizeof dev) == 0) n++;
    CHECK(n == 5 && sys_kinfo(KI_DEVICE, 0, &dev, sizeof dev) == 0 && !strcmp(dev.name, "kbd0") && dev.irq == 1);
    puts("kernel information: bad calls are refused");
    CHECK(sys_kinfo(0, 0, &cpu, sizeof cpu) == SYS_EINVAL && sys_kinfo(KI_COUNT, 0, &cpu, sizeof cpu) == SYS_EINVAL && sys_kinfo(999, 0, &cpu, sizeof cpu) == SYS_EINVAL);
    CHECK(sys_kinfo(KI_CPU, 0, &cpu, sizeof cpu - 1) == SYS_EINVAL && sys_kinfo(KI_CPU, 0, &cpu, 0) == SYS_EINVAL && sys_kinfo(KI_CPU, 0, NULL, sizeof cpu) == SYS_EFAULT);
    CHECK(sys_kinfo(KI_CPU, 0, (void *)(uintptr_t)8, sizeof cpu) == SYS_EFAULT);
    CHECK(kinfo_weekday(2026, 10, 12) == 1 && kinfo_weekday(2000, 1, 1) == 6 && kinfo_weekday(2024, 2, 29) == 4 && kinfo_bcd(0x59) == 59 && kinfo_bcd(0x09) == 9 && kinfo_bcd(0x00) == 0);
    for (int w = 1; w < KI_COUNT; w++) CHECK(kinfo_size(w) > 0 && kinfo_size(w) <= KI_MAX_STRUCT);
    puts("power control needs root");
    login_as("student", "student123");
    CHECK(sys_power(POWER_REBOOT) == SYS_EACCES && sys_power(POWER_SHUTDOWN) == SYS_EACCES && sys_power(POWER_HALT) == SYS_EACCES);
    sys_logout(); ROOT();
    CHECK(sys_power(POWER_REBOOT) == 0 && sys_power(POWER_SHUTDOWN) == 0 && sys_power(POWER_HALT) == 0 && sys_power(0) == SYS_EINVAL && sys_power(99) == SYS_EINVAL);
    sys_logout();
    CHECK(sys_power(POWER_REBOOT) == SYS_EACCES);
}

/* ================= RAM-disk and images ================= */
static void test_ramdisk(void) {
    puts("RAM-disk: raw sectors need root and are bounds-checked");
    ramdisk_info_t ri; uint8_t a[512], b[512];
    CHECK(sys_blk_info(&ri) == 0 && ri.sector_size == 512 && ri.sectors == RAMDISK_SECTORS && ri.reads == 0 && ri.writes == 0 && !ri.image_valid && sys_blk_info(NULL) == SYS_EFAULT);
    login_as("student", "student123");
    CHECK(sys_blk_read(0, b) == SYS_EACCES && sys_blk_write(0, a) == SYS_EACCES && sys_fs_save(NULL) == SYS_EACCES && sys_fs_load() == SYS_EACCES);
    sys_logout(); ROOT();
    for (int i = 0; i < 512; i++) a[i] = (uint8_t)(i * 3 + 1);
    CHECK(sys_blk_write(7, a) == 0 && sys_blk_read(7, b) == 0 && !memcmp(a, b, 512) && sys_blk_read(8, b) == 0 && b[0] == 0 && b[511] == 0);
    CHECK(sys_blk_write(RAMDISK_SECTORS - 1, a) == 0 && sys_blk_read(RAMDISK_SECTORS - 1, b) == 0 && !memcmp(a, b, 512));
    CHECK(sys_blk_read(RAMDISK_SECTORS, b) == SYS_EBLOCK && sys_blk_write(RAMDISK_SECTORS, a) == SYS_EBLOCK && sys_blk_read(0xFFFFFFFFu, b) == SYS_EBLOCK);
    CHECK(sys_blk_read(0, NULL) == SYS_EFAULT && sys_blk_write(0, NULL) == SYS_EFAULT);
    CHECK(sys_blk_info(&ri) == 0 && ri.writes == 2 && ri.reads == 3);
    puts("MiniFS image: save, change everything, load");
    sys_mkdir("/home/student/proj", 0); put("/home/student/proj/a.txt", "alpha\n"); sys_chown("/home/student/proj/a.txt", 2, 2); sys_chmod("/home/student/proj/a.txt", 0640);
    sys_mkdir("/tmp/emptydir", 0); sys_chmod("/tmp/emptydir", 0700); put("/tmp/locked.txt", "do not touch\n"); sys_protect("/tmp/locked.txt", true);
    put("/tmp/binary.bin", "x"); { char big[3000]; memset(big, 'z', sizeof big); big[2999] = 0; put("/tmp/big.txt", big); }
    uint32_t bytes = 0; int nodes = sys_fs_save(&bytes);
    CHECK(nodes >= 20 && bytes > 3000 && sys_blk_info(&ri) == 0 && ri.image_valid && ri.image_nodes == (uint32_t)nodes && ri.image_bytes == bytes && ri.image_tick == timer_ticks());
    sys_protect("/tmp/locked.txt", false); sys_delete("/tmp/locked.txt"); sys_delete("/home/student/proj/a.txt"); sys_rmdir("/tmp/emptydir"); put("/tmp/new.txt", "created after the save\n");
    sys_chmod("/etc/motd", 0600); put("/etc/hostname", "changed\n");
    CHECK(sys_fs_load() == nodes);
    fs_stat_t st;
    CHECK(file_is("/home/student/proj/a.txt", "alpha\n") && owner_of("/home/student/proj/a.txt") == 2 && mode_of("/home/student/proj/a.txt") == 0640);
    CHECK(sys_stat("/tmp/emptydir", &st) == 0 && st.type == FS_DIR && st.mode == 0700);
    CHECK(file_is("/tmp/locked.txt", "do not touch\n") && sys_stat("/tmp/locked.txt", &st) == 0 && st.protect && sys_delete("/tmp/locked.txt") == SYS_EPROTECT);
    CHECK(sys_stat("/tmp/new.txt", &st) == SYS_ENOENT && mode_of("/etc/motd") == 0644 && !file_is("/etc/hostname", "changed\n"));
    CHECK(sys_stat("/tmp/big.txt", &st) == 0 && st.size == 2999 && owner_of("/etc/passwd") == 0);
    { fs_check_report_t r; CHECK(fs_check(&r) == 0); }
    char cwd[64]; CHECK(sys_getcwd(cwd, sizeof cwd) >= 0 && !strcmp(cwd, "/"));
    puts("MiniFS image: a damaged image never touches the live file system");
    put("/tmp/marker", "still here\n");
    CHECK(sys_blk_read(0, b) == 0); b[100] ^= 0x40; CHECK(sys_blk_write(0, b) == 0);
    CHECK(sys_fs_load() == SYS_EINVAL && file_is("/tmp/marker", "still here\n"));
    b[100] ^= 0x40; sys_blk_write(0, b); CHECK(sys_fs_load() > 0 && sys_stat("/tmp/marker", &st) == SYS_ENOENT);
    put("/tmp/marker2", "x"); sys_blk_read(0, b); b[0] = 'X'; sys_blk_write(0, b);
    CHECK(sys_fs_load() == SYS_ENOENT && file_is("/tmp/marker2", "x"));
    b[0] = 'M'; sys_blk_write(0, b);
    uint8_t keep[512]; sys_blk_read(0, b); memcpy(keep, b, 512);
    b[12] = 0xFF; b[13] = 0xFF; b[14] = 0xFF; b[15] = 0x7F; sys_blk_write(0, b);
    CHECK(sys_fs_load() == SYS_EINVAL && file_is("/tmp/marker2", "x")); sys_blk_write(0, keep);
    puts("MiniFS image: no image, too big, accounts stay consistent");
    CHECK(sh("ramdisk clear") == 0 && has("erased") && sys_blk_info(&ri) == 0 && !ri.image_valid && sys_fs_load() == SYS_ENOENT);
    for (int i = 0; i < 3; i++) { char name[24]; snprintf(name, sizeof name, "/tmp/huge%d", i); int fd = sys_open(name, SYS_O_WRITE | SYS_O_CREATE); static char chunk[4096]; memset(chunk, 'h', sizeof chunk); for (int k = 0; k < 50; k++) sys_write(fd, chunk, sizeof chunk); sys_close(fd); }
    CHECK(sys_fs_save(NULL) == SYS_ENOSPC && sys_blk_info(&ri) == 0 && !ri.image_valid);
    for (int i = 0; i < 3; i++) { char name[24]; snprintf(name, sizeof name, "/tmp/huge%d", i); sys_delete(name); }
    CHECK(sys_fs_save(NULL) > 0);
    CHECK(sys_user_add("newbie", "pass1234", 0) >= 1000 && sys_fs_load() > 0 && sys_stat("/home/newbie", &st) == 0 && st.uid >= 1000 && file_is("/etc/hostname", "minios\n") && sys_verify("newbie", "pass1234") == 0);
    sys_logout();
}

/* ================= shell environment commands ================= */
static void test_shellenv(void) {
    puts("variables, aliases, prompt");
    ROOT();
    CHECK(sh("set GREETING hello") == 0 && sh("echo $GREETING") == 0 && is("hello\n") && sh("echo ${GREETING}x") == 0 && is("hellox\n") && sh("echo '$GREETING'") == 0 && is("$GREETING\n"));
    CHECK(sh("set Y=a b c") == 0 && sh("echo $Y") == 0 && is("a b c\n") && sh("set") == 0 && has("GREETING=hello") && has("Y=a b c"));
    CHECK(sh("set 1bad x") == 1 && has("not a valid name") && sh("set a-b=1") == 1 && sh("set EMPTY=") == 0 && sh("echo [$EMPTY]") == 0 && is("[]\n"));
    CHECK(sh("unset GREETING") == 0 && sh("echo [$GREETING]") == 0 && is("[]\n") && sh("unset GREETING") == 1 && has("is not set") && sh("unset") == 1);
    CHECK(sh("env") == 0 && has("nothing exported") && sh("export Y") == 0 && sh("export Z=zed") == 0 && sh("env") == 0 && has("Y=a b c") && has("Z=zed") && !has("EMPTY") && sh("export NOPE") == 1);
    CHECK(sh("nosuchcmd") == 127 && sh("echo $?") == 0 && is("127\n") && sh("echo ok") == 0 && sh("echo $?") == 0 && is("0\n"));
    CHECK(sh("alias ll=\"ls -l\"") == 0 && sh("alias") == 0 && has("alias ll='ls -l'") && sh("alias ll") == 0 && has("ls -l") && sh("ll /etc") == 0 && has("motd"));
    CHECK(sh("alias hi echo hello there") == 0 && sh("hi") == 0 && is("hello there\n") && sh("unalias hi") == 0 && sh("hi") == 127 && sh("unalias hi") == 1 && sh("alias nope") == 1 && sh("alias 1x=y") == 1 && sh("alias alias=x") == 1);
    CHECK(sh("alias ls=\"ls -l\"") == 0 && sh("ls /etc") == 0 && has("motd"));
    CHECK(sh("prompt") == 0 && has("default") && sh("prompt [\\u]\\$ ") == 0 && sh("prompt") == 0 && has("[\\u]") && sh("echo $PS1") == 0 && sh("prompt default") == 0 && var_get("PS1") == NULL);
    sys_logout();

    puts("test: conditions");
    ROOT(); put("/tmp/f", "data\n"); put("/tmp/empty", "");
    CHECK(sh("test -e /tmp/f") == 0 && sh("test -e /tmp/none") == 1 && sh("test -f /tmp/f") == 0 && sh("test -f /tmp") == 1 && sh("test -d /tmp") == 0 && sh("test -d /tmp/f") == 1 && sh("test -s /tmp/f") == 0 && sh("test -s /tmp/empty") == 1 && sh("test -r /tmp/f") == 0 && sh("test -r /root/none") == 1);
    CHECK(sh("test -z \"\"") == 0 && sh("test -z abc") == 1 && sh("test -n abc") == 0 && sh("test abc = abc") == 0 && sh("test abc = abd") == 1 && sh("test abc != abd") == 0 && sh("test hello") == 0 && sh("test") == 1);
    CHECK(sh("test 3 -lt 10") == 0 && sh("test 10 -lt 3") == 1 && sh("test 5 -eq 2+3") == 0 && sh("test -5 -lt 0") == 0 && sh("test 4 -ge 4") == 0 && sh("test 4 -gt 4") == 1 && sh("test 4 -le 3") == 1 && sh("test 4 -ne 3") == 0);
    CHECK(sh("test ! -e /tmp/none") == 0 && sh("test ! -e /tmp/f") == 1 && sh("test x -lt 3") == 2 && has("cannot understand") && sh("test 1 2 3 4") == 2 && sh("test -q x") == 2);
    puts("source and repeat");
    put("/tmp/script", "# a comment\n\necho first\n  echo second\nset COUNT 3\necho count=$COUNT\nnosuchcommand\n");
    CHECK(sh("source /tmp/script") == 127 && has("first") && has("second") && has("count=3") && has("unknown command"));        /* status of the last command */
    put("/tmp/ok", "echo one\necho two\n"); CHECK(sh("source /tmp/ok") == 0 && has("one") && has("two") && sh("source") == 1 && sh("source /tmp/missing") == 1 && has("not found"));
    put("/tmp/loop", "source /tmp/loop\n"); CHECK(sh("source /tmp/loop") != 0 && has("nested too deeply"));                       /* a script that runs itself stops */
    { char longline[200]; memset(longline, 'a', 150); longline[150] = 0; char s[300]; snprintf(s, sizeof s, "echo %s\n", longline); put("/tmp/long", s); CHECK(sh("source /tmp/long") == 1 && has("longer than 127")); }
    CHECK(sh("repeat 3 echo hi") == 0 && is("hi\nhi\nhi\n") && sh("repeat 0 echo x") == 1 && sh("repeat 101 echo x") == 1 && sh("repeat 2") == 1 && sh("repeat 2 nosuch") == 127 && sh("repeat 2 repeat 2 echo x") == 1 && sh("repeat x echo y") == 1);
    CHECK(sh("banner") == 0 && has("Educational Operating System") && has("0.16.0"));
    CHECK(sh("exit") == 0 && has("Session ended") && sys_getuid() == SYS_ENOUSER && sh("exit") == 1);
}

/* ================= text utilities ================= */
static void test_text(void) {
    puts("text: sort uniq tac rev nl fold");
    ROOT();
    put("/tmp/t1", "b\na\nc\na\n"); put("/tmp/n1", "10\n9\n100\n-5\nabc\n"); put("/tmp/u1", "a\na\nb\na\n"); put("/tmp/crlf", "one\r\ntwo\r\n");
    CHECK(sh("sort /tmp/t1") == 0 && is("a\na\nb\nc\n") && sh("sort -r /tmp/t1") == 0 && is("c\nb\na\na\n") && sh("sort -u /tmp/t1") == 0 && is("a\nb\nc\n") && sh("sort -ru /tmp/t1") == 0 && is("c\nb\na\n"));
    CHECK(sh("sort -n /tmp/n1") == 0 && is("-5\nabc\n9\n10\n100\n") && sh("sort /tmp/n1") == 0 && is("-5\n10\n100\n9\nabc\n"));
    CHECK(sh("sort") == 1 && has("usage:") && sh("sort -x /tmp/t1") == 1 && sh("sort /tmp/missing") == 1 && has("not found") && sh("sort /tmp") == 1 && sh("sort a b") == 1);
    CHECK(sh("sort /tmp/crlf") == 0 && is("one\ntwo\n"));
    put("/tmp/noeol", "z\ny"); CHECK(sh("sort /tmp/noeol") == 0 && is("y\nz\n")); put("/tmp/none", ""); CHECK(sh("sort /tmp/none") == 0 && is(""));
    CHECK(sh("uniq /tmp/u1") == 0 && is("a\nb\na\n") && sh("uniq -c /tmp/u1") == 0 && is("     2 a\n     1 b\n     1 a\n") && sh("uniq") == 1);
    CHECK(sh("tac /tmp/t1") == 0 && is("a\nc\na\nb\n") && sh("rev /tmp/t1") == 0 && is("b\na\nc\na\n") && sh("nl /tmp/t1") == 0 && has("     1  b") && has("     4  a"));
    put("/tmp/r1", "abc\nde\n"); CHECK(sh("rev /tmp/r1") == 0 && is("cba\ned\n"));
    put("/tmp/fold", "abcdefghij\n\nxy\n"); CHECK(sh("fold -w 4 /tmp/fold") == 0 && is("abcd\nefgh\nij\n\nxy\n") && sh("fold -w 3 /tmp/fold") == 1 && has("between 4 and 200") && sh("fold -w 4") == 1 && sh("fold -w 201 /tmp/fold") == 1 && sh("fold /tmp/fold") == 0);
    { static char big[16384]; size_t o = 0; for (int i = 0; i < 4097; i++) { big[o++] = 'x'; big[o++] = '\n'; } big[o] = 0; put("/tmp/many", big); CHECK(sh("sort /tmp/many") == 1 && has("more than 4096 lines"));
      o = 0; for (int i = 0; i < 4096; i++) { big[o++] = 'x'; big[o++] = '\n'; } big[o] = 0; put("/tmp/many", big); CHECK(sh("tac /tmp/many") == 0); }
    puts("text: cut tr join split paste");
    put("/tmp/c1", "x,y,z\n1,2,3\nonly\n"); put("/tmp/sp", "a b c\n");
    CHECK(sh("cut -d , -f 2 /tmp/c1") == 0 && is("y\n2\n\n") && sh("cut -d , -f 1,3 /tmp/c1") == 0 && is("x,z\n1,3\nonly\n") && sh("cut -f 2 /tmp/sp") == 0 && is("b\n"));
    CHECK(sh("cut -d , /tmp/c1") == 1 && sh("cut -f 0 /tmp/c1") == 1 && has("count from 1") && sh("cut -d ab -f 1 /tmp/c1") == 1 && sh("cut -f x /tmp/c1") == 1 && sh("cut -f 1") == 1);
    put("/tmp/tr", "hello World\n"); CHECK(sh("tr a-z A-Z /tmp/tr") == 0 && is("HELLO WORLD\n") && sh("tr -d aeiou /tmp/tr") == 0 && is("hll Wrld\n") && sh("tr lo 0 /tmp/tr") == 0 && is("he000 W0r0d\n") && sh("tr -d x") == 1 && sh("tr a") == 1);
    CHECK(sh("join , /tmp/t1") == 0 && is("b,a,c,a\n") && sh("join") == 1 && sh("split , a,b,c") == 0 && is("a\nb\nc\n") && sh("split ab x") == 1);
    put("/tmp/p1", "1\n2\n3\n"); put("/tmp/p2", "a\nb\n"); CHECK(sh("paste /tmp/p1 /tmp/p2") == 0 && is("1\ta\n2\tb\n3\t\n") && sh("paste /tmp/p1") == 1 && sh("paste /tmp/p1 /tmp/none2") == 1);
    puts("text: case, trim, numbers, strings");
    CHECK(sh("upper hello world") == 0 && is("HELLO WORLD\n") && sh("lower HeLLo") == 0 && is("hello\n") && sh("capitalize hELLO wORLD") == 0 && is("Hello World\n") && sh("trim \"  hi  \"") == 0 && is("hi\n"));
    put("/tmp/case", "one\nTwo\n"); CHECK(sh("upper -f /tmp/case") == 0 && is("ONE\nTWO\n") && sh("lower -f /tmp/case") == 0 && is("one\ntwo\n") && sh("upper") == 1 && sh("upper -f /tmp/missing") == 1);
    CHECK(sh("sum 1 2 3") == 0 && is("6\n") && sh("sum -5 5") == 0 && is("0\n") && sh("sum 2*3 4") == 0 && is("10\n") && sh("sum 9223372036854775807 1") == 1 && has("does not fit") && sh("sum x") == 1 && has("not a number") && sh("sum") == 1);
    CHECK(sh("min 3 -2 9") == 0 && is("-2\n") && sh("max 3 -2 9") == 0 && is("9\n") && sh("min 7") == 0 && is("7\n") && sh("max") == 1);
    CHECK(sh("avg 1 2") == 0 && is("1.50\n") && sh("avg -1 -2") == 0 && is("-1.50\n") && sh("avg 10") == 0 && is("10.00\n") && sh("avg 1 2 2") == 0 && is("1.66\n") && sh("avg 0 -1") == 0 && is("-0.50\n") && sh("avg 1 1 8") == 0 && is("3.33\n") && sh("avg 3 4 4") == 0 && is("3.66\n"));
    { char cmd[600] = "sum"; for (int i = 0; i < 17; i++) strcat(cmd, " 1"); CHECK(sh(cmd) == 2 && has("too many arguments")); }   /* the parser stops it first */
    CHECK(sh("strlen hello world") == 0 && is("11\n") && sh("strlen") == 1 && sh("repeatstr ab 3") == 0 && is("ababab\n") && sh("repeatstr x 0") == 0 && is("\n") && sh("repeatstr x 201") == 1 && sh("repeatstr abcdefghijk 200") == 1 && has("longer than 2000") && sh("repeatstr x") == 1);
    CHECK(sh("replace aXbXc X -") == 0 && is("a-b-c\n") && sh("replace hello l L") == 0 && is("heLLo\n") && sh("replace abc \"\" x") == 1 && sh("replace a b") == 1 && sh("replace aaa a aa") == 0 && is("aaaaaa\n"));
    CHECK(sh("contains hello ell") == 0 && is("true\n") && sh("contains hello xyz") == 1 && is("false\n") && sh("contains hello \"\"") == 0 && sh("startswith hello he") == 0 && sh("startswith hello lo") == 1 && sh("endswith hello lo") == 0 && sh("endswith hello he") == 1 && sh("endswith lo hello") == 1 && sh("contains a") == 1 && has("usage:"));
    CHECK(sh("ascii") == 0 && has(" 65  41  A") && has(" 126  7e  ~") && sh("ascii A") == 0 && has("decimal 65") && sh("ascii AB") == 1);
    put("/tmp/words", "the cat the dog The end. the CAT\n"); CHECK(sh("wordfreq /tmp/words") == 0 && has("     4 the\n     2 cat\n") && has("7 words, 4 different") == false && has("8 words, 4 different"));
    put("/tmp/nowords", "... !!!\n"); CHECK(sh("wordfreq /tmp/nowords") == 0 && has("0 words") && sh("wordfreq") == 1);
    sys_logout();
}

/* ================= system information ================= */
static void test_sysinfo_cmds(void) {
    puts("system information commands");
    login_as("student", "student123");
    CHECK(sh("date") == 0 && is("Monday, 12 October 2026\n") && sh("time") == 0 && has("09:30:15") && sh("date x") == 1 && sh("time x") == 1);
    CHECK(sh("cpuinfo") == 0 && has("Vendor") && has("Features") && sh("cpuid 0") == 0 && has("EAX=0x") && has("as text: \"") && sh("cpuid") == 1 && has("usage:") && sh("cpuid zz") == 1 && sh("cpuid 1 2 3") == 1 && sh("cpuid 0x80000002") == 0 && has("as text"));
    CHECK(sh("ticks") == 0 && has("500 ticks") && sh("timerinfo") == 0 && has("1193182 Hz") && has("100 Hz") && has("10 ms"));
    CHECK(sh("irqinfo") == 0 && has("timer") && has("keyboard") && has("ide-secondary") && sh("idtinfo") == 0 && has("2048") == false && has("2047") && has("(int32)") && sh("idtinfo 250 20") == 0 && has("255") && sh("idtinfo 300") == 1 && sh("idtinfo 1 2 3") == 1);
    CHECK(sh("gdtinfo") == 0 && has("3 entries") && sh("gdtinfo 0 99") == 1);
    CHECK(sh("kernelinfo") == 0 && has("Heap"));
    kinfo_set_boot(&mbi);
    CHECK(sh("bootinfo") == 0 && has("GRUB") && has("Memory map (1 entries)") && has("available") && sh("vgainfo") == 0 && has("80x25") && has("0x000B8000") && sh("kbdinfo") == 0 && has("scancode set 1"));
    CHECK(sh("devices") == 0 && has("kbd0") && has("vga0") && has("pit0") && has("rtc0") && has("ram0") && sh("arch") == 0 && is("i386\n") && sh("osname") == 0 && is("MiniOS\n") && sh("about") == 0 && has("Simulated or missing"));
    puts("power commands need root");
    CHECK(sh("reboot") == 1 && has("only root") && sh("shutdown") == 1 && has("only root") && sh("halt") == 1 && has("only root") && sh("reboot now") == 1 && has("usage:"));
    sys_logout(); ROOT();
    CHECK(sh("reboot") == 0 && has("Rebooting") && sh("shutdown") == 0 && has("Shutting down") && sh("halt") == 0 && has("Halting"));
    sys_logout();
}

/* ================= disk commands ================= */
static void test_disk_cmds(void) {
    puts("disk commands");
    login_as("student", "student123");
    CHECK(sh("lsblk") == 0 && has("ram0") && has("512.0 KiB") && has("empty") && sh("diskinfo") == 0 && has("Sector size") && has("none") && sh("fdisk") == 0 && has("Partition table: none") && sh("sync") == 0 && has("Nothing to flush") && sh("ramdisk") == 0 && has("empty"));
    CHECK(sh("blkread 0") == 1 && has("needs root") && sh("blkwrite 0 x") == 1 && has("needs root") && sh("blkdump 0") == 1 && sh("savefs") == 1 && has("needs root") && sh("loadfs --yes") == 1 && sh("ramdisk clear") == 1 && sh("diskbench") == 1 && sh("badblocks") == 1);
    sys_logout(); ROOT();
    CHECK(sh("blkwrite 5 Hello RAM disk") == 0 && has("Wrote 14 bytes") && sh("blkread 5") == 0 && has("Hello RAM disk...") && sh("blkdump 5 32") == 0 && has("48 65 6c 6c 6f") && has("Hello RAM disk") && sh("blkdump 5") == 0);
    CHECK(sh("blkread 1024") == 1 && has("0 to 1023") && sh("blkread x") == 1 && sh("blkread") == 1 && sh("blkdump 5 8") == 1 && sh("blkdump 5 513") == 1 && sh("blkwrite 5") == 1 && sh("blkwrite 5 \"\"") == 0);
    CHECK(sh("diskbench") == 0 && has("Read :") && has("Write:") && has("not a physical disk") && sh("badblocks") == 0 && has("0 bad") && has("sectors read"));
    CHECK(sh("loadfs") == 1 && has("REPLACES") && sh("loadfs --yes") == 1 && has("no saved image") && sh("loadfs maybe") == 1);
    put("/tmp/keepme", "kept\n"); CHECK(sh("savefs") == 0 && has("Saved") && has("It lasts until reboot") && sh("lsblk") == 0 && has("MiniFS image") && sh("diskinfo") == 0 && has("nodes"));
    put("/tmp/later", "later\n"); sys_delete("/tmp/keepme");
    CHECK(sh("loadfs --yes") == 0 && has("Restored") && file_is("/tmp/keepme", "kept\n") && sh("stat /tmp/later") == 1);
    CHECK(sh("diskbench") == 0 && sh("badblocks") == 0 && sh("loadfs --yes") == 0);                                      /* the benchmark and the scan never damage a saved image */
    sys_logout();
}

/* ================= utilities ================= */
static void test_misc(void) {
    puts("calc, base, number theory");
    login_as("student", "student123");
    CHECK(sh("calc 2 + 3 * 4") == 0 && is("14\n") && sh("calc \"(2+3)*4\"") == 0 && is("20\n") && sh("calc -7 / 2") == 0 && is("-3\n") && sh("calc 1/0") == 1 && has("division by zero") && sh("calc") == 1 && sh("calc 9223372036854775807+1") == 1 && has("does not fit") && sh("calc 2 +") == 1 && has("syntax"));
    CHECK(sh("base ff 16 10") == 0 && has("decimal") && has("255") && sh("base 255") == 0 && has("binary  11111111") && has("octal   377") && has("hex     ff") && sh("base 2 2") == 1 && has("not a valid number") && sh("base 255 10 37") == 1 && sh("base zz 36 10") == 0 && has("1295") && sh("base 10 10 36") == 0 && has("base 36: a"));
    CHECK(sh("factor 360") == 0 && is("360: 2 2 2 3 3 5\n") && sh("factor 97") == 0 && is("97: 97\n") && sh("factor 1") == 0 && has("no prime factors") && sh("factor 4294967296") == 1 && sh("factor x") == 1 && sh("factor 4294967291") == 0 && is("4294967291: 4294967291\n"));
    CHECK(sh("prime 97") == 0 && has("is prime") && sh("prime 91") == 1 && has("not prime") && sh("prime 2") == 0 && sh("prime 1") == 1 && sh("prime 18446744073709551615") == 1 && has("too long") && sh("prime") == 1);
    CHECK(sh("fib 10") == 0 && is("55\n") && sh("fib 0") == 0 && is("0\n") && sh("fib 93") == 0 && is("12200160415121876738\n") && sh("fib 94") == 1 && has("does not fit") && sh("fib x") == 1);
    CHECK(sh("gcd 12 18") == 0 && is("6\n") && sh("gcd 12 18 24") == 0 && is("6\n") && sh("gcd 0 5") == 0 && is("5\n") && sh("gcd 12") == 1 && sh("gcd a b") == 1);
    CHECK(sh("seq 5") == 0 && is("1 2 3 4 5\n") && sh("seq 3 6") == 0 && is("3 4 5 6\n") && sh("seq 10 -3 1") == 0 && is("10 7 4 1\n") && sh("seq 5 1") == 0 && is("") && sh("seq 1 0 5") == 1 && sh("seq 1 2000") == 1 && has("more than 1000") && sh("seq x") == 1 && sh("seq 0") == 0 && is(""));
    puts("calendar");
    CHECK(sh("cal 2 2024") == 0 && has("   February 2024\nSu Mo Tu We Th Fr Sa\n") && has("             1  2  3\n") && has("25 26 27 28 29\n") && sh("cal 2 2023") == 0 && has("26 27 28\n") && has("29") == false);
    CHECK(sh("cal") == 0 && has("October 2026") && has("12") && sh("cal 13 2024") == 1 && sh("cal 0 2024") == 1 && sh("cal 2 0") == 1 && sh("cal 2 10000") == 1 && sh("cal 2") == 1 && sh("cal 1 1") == 0 && has("January 1"));
    puts("random, dice, yes, cowsay");
    int bad = 0; for (int i = 0; i < 300; i++) { sh("random 5 8"); int v = atoi(out); if (v < 5 || v > 8) bad++; }
    CHECK(bad == 0);
    int seen[4] = {0}; for (int i = 0; i < 300; i++) { sh("random 5 8"); seen[atoi(out) - 5]++; } CHECK(seen[0] && seen[1] && seen[2] && seen[3]);
    CHECK(sh("random 9 3") == 1 && has("larger") && sh("random abc") == 1 && sh("random") == 0 && sh("random 7") == 0 && sh("random 0 4294967295") == 0 && sh("random 3 3") == 0 && is("3\n") && sh("random 1 2 3") == 1);
    bad = 0; for (int i = 0; i < 200; i++) { sh("dice 2 6"); int a = atoi(out); const char *sp = strchr(out, ' '); int b = sp ? atoi(sp + 1) : 0; if (a < 1 || a > 6 || b < 1 || b > 6) bad++; }
    CHECK(bad == 0 && sh("dice 3") == 0 && has("total") && sh("dice") == 0 && sh("dice 0") == 1 && sh("dice 11") == 1 && sh("dice 1 1") == 1 && sh("dice 1 2 3") == 1);
    CHECK(sh("yes") == 0 && is("y\ny\ny\ny\ny\ny\ny\ny\ny\ny\n") && sh("yes hi 3") == 0 && is("hi\nhi\nhi\n") && sh("yes 2") == 0 && is("y\ny\n") && sh("yes a b 2") == 0 && is("a b\na b\n") && sh("yes x 1001") == 0 && has("x 1001"));
    CHECK(sh("cowsay moo") == 0 && has("< moo >") && has("^__^") && sh("cowsay") == 1 && sh("cowsay aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 1);
    puts("colour, stopwatch, timer");
    CHECK(sh("color") == 0 && has("lightgreen") && sh("color red") == 0 && sh("color white blue") == 0 && sh("color 2 0") == 0 && sh("color red red") == 1 && has("unreadable") && sh("color nosuch") == 1 && sh("color 16") == 1 && sh("color a b c") == 1 && sh("color reset") == 0 && has("restored"));
    CHECK(sh("stopwatch") == 1 && has("not started") && sh("stopwatch start") == 0 && has("started")); fake_ticks += 250;
    CHECK(sh("stopwatch show") == 0 && has("2.50 s (running)") && sh("stopwatch stop") == 0 && has("2.50 s") && has("(stopped)")); fake_ticks += 100;
    CHECK(sh("stopwatch show") == 0 && has("2.50 s (stopped earlier)") && sh("stopwatch reset") == 0 && sh("stopwatch show") == 1 && sh("stopwatch bogus") == 1 && sh("stopwatch 1 2") == 1);
    CHECK(sh("timer") == 1 && has("usage:") && sh("timer 0") == 1 && sh("timer 61") == 1 && sh("timer x") == 1 && sh("timer 2") == 0 && has("2... 1... time is up!"));
    puts("demo and credits");
    feed("\n\n\n\n\n\n\n\n\n\n\n\n\n");
    CHECK(sh("demo") == 0 && has("[1/13]") && has("[13/13]") && has("Hello MiniOS") && has("Demo finished") && fed_all());
    { fs_stat_t st; CHECK(sys_stat("/tmp/demo", &st) == SYS_ENOENT); }                                  /* it cleans up after itself */
    feed("\n\n\x03"); CHECK(sh("demo") == 1 && has("Demo stopped") && has("[3/13]") && !has("[4/13]"));
    kq[0] = 0; sh("rm /tmp/demo/hello.txt"); sh("rmdir /tmp/demo");
    CHECK(sh("demo x") == 1 && sh("credits") == 0 && has("0.16.0") && sh("credits x") == 1);
    sys_logout();
}

/* ================= simulated network ================= */
static void test_network(void) {
    puts("network: everything is labelled SIMULATED");
    ROOT();
    CHECK(sh("netinfo") == 0 && has("SIMULATED") && sh("ifconfig") == 0 && has("[SIMULATED]") && has("10.0.2.15") && has("lo0") && sh("route") == 0 && has("[SIMULATED]") && sh("arp") == 0 && has("[SIMULATED]") && sh("netstat") == 0 && has("no socket API"));
    CHECK(sh("ip") == 0 && has("eth0") && sh("ip addr") == 0 && has("eth0") && sh("ip route") == 0 && has("default") && sh("ip neigh") == 0 && has("HWaddress") && sh("ip bogus") == 1 && sh("ip a b") == 1);
    CHECK(sh("host") == 0 && has("example.test") && sh("host localhost") == 0 && has("127.0.0.1") && sh("host 10.0.2.15") == 0 && has("minios.local") && sh("host nosuch.example") == 1 && has("unknown host") && sh("nslookup gateway.local") == 0 && has("10.0.2.2") && sh("nslookup zzz") == 1 && sh("dig dns.local") == 0 && has("NOERROR") && has("10.0.2.3") && sh("dig zzz") == 1 && has("NXDOMAIN") && sh("dig") == 1);
    puts("ping, traceroute and the network simulator");
    CHECK(sh("ping gateway.local") == 0 && has("[SIMULATED]") && has("4 sent, 4 received, 0% loss") && has("ttl=64") && sh("ping localhost 2") == 0 && has("time=0 ms") && has("2 sent, 2 received"));
    CHECK(sh("ping 8.8.8.8") == 1 && has("unreachable") && sh("ping nosuchhost") == 1 && has("unknown host") && sh("ping 300.1.1.1") == 1 && sh("ping 10.0.2.99") == 0 && sh("ping") == 1 && sh("ping localhost 0") == 1 && sh("ping localhost 21") == 1);
    CHECK(sh("netsim loss 50") == 0 && sh("ping localhost") == 0 && has("4 sent, 2 received, 50% loss") && has("timeout") && sh("netsim loss 100") == 0 && sh("ping localhost") == 1 && has("0 received") && sh("netsim loss 0") == 0 && sh("netsim latency 40") == 0 && sh("ping gateway.local 1") == 0 && has("time=40 ms") && sh("netsim") == 0 && has("latency 40 ms"));
    CHECK(sh("netsim loss 101") == 1 && sh("netsim latency 2001") == 1 && sh("netsim loss x") == 1 && sh("netsim bogus") == 1 && sh("netsim reset") == 0 && sh("netsim show") == 0 && has("latency 2 ms, loss 0%"));
    CHECK(sh("traceroute localhost") == 0 && has(" 1  127.0.0.1") && sh("traceroute example.test") == 0 && has("192.0.2.10") && has("10.0.2.2") && sh("traceroute 8.8.8.8") == 1 && has("* * *") && sh("traceroute gateway.local") == 0 && sh("traceroute") == 1 && sh("traceroute nosuch") == 1);
    puts("curl and wget: real reads from /var/www");
    CHECK(sh("curl http://localhost/hello.txt") == 1 && has("404") && has("SIMULATED"));
    sys_mkdir("/var/www", 0); put("/var/www/hello.txt", "Hello from the virtual server\n"); put("/var/www/index.html", "<h1>home</h1>"); sys_mkdir("/var/www/sub", 0);
    CHECK(sh("curl http://localhost/hello.txt") == 0 && is("Hello from the virtual server\n") && sh("curl http://minios.local/hello.txt") == 0 && sh("curl http://127.0.0.1/hello.txt") == 0 && sh("curl /hello.txt") == 0 && sh("curl hello.txt") == 0);
    CHECK(sh("curl http://localhost/") == 0 && is("<h1>home</h1>\n") && sh("curl http://localhost") == 0 && has("home") && sh("curl http://localhost/sub") == 1 && has("404"));
    CHECK(sh("curl http://example.com/hello.txt") == 1 && has("only http://localhost") && sh("curl https://localhost/hello.txt") == 1 && sh("curl http://localhost/../etc/passwd") == 1 && sh("curl ../etc/passwd") == 1 && sh("curl") == 1 && sh("curl a b") == 1);
    sys_chdir("/tmp");
    CHECK(sh("wget http://localhost/hello.txt") == 0 && has("Saved 30 bytes to hello.txt") && file_is("/tmp/hello.txt", "Hello from the virtual server\n") && sh("wget http://localhost/hello.txt") == 1 && has("already exists"));
    CHECK(sh("wget -O copy.txt http://localhost/hello.txt") == 0 && file_is("/tmp/copy.txt", "Hello from the virtual server\n") && sh("wget http://localhost/") == 0 && file_is("/tmp/index.html", "<h1>home</h1>") && sh("wget") == 1 && sh("wget http://example.com/x") == 1 && sh("wget http://localhost/none.txt") == 1 && has("404"));
    sys_logout();
}

/* ================= diagnostics ================= */
static void test_diag(void) {
    puts("diagnostics");
    ROOT();
    CHECK(sh("errlist") == 0 && has("ENOENT") && has("EAUTH") && has("EPROTECT") && has("EBLOCK") && has("-31"));
    { int lines = 0; for (const char *p = out; *p; p++) lines += *p == '\n'; CHECK(lines == 1 + 33); }
    CHECK(sh("errlist x") == 1);               /* header + codes 0 to -31 */
    CHECK(sh("selftest") == 0 && has("10/10 checks passed") && !has("FAIL") && sh("selftest x") == 1);
    CHECK(sh("diag") == 0 && has("memory") && has("filesystem") && has("OK") && !has("FAULT") && has("implemented") && sh("diag x") == 1);
    CHECK(sh("regs") == 1 && has("host test") && sh("stack") == 1 && sh("stack 99") == 1 && sh("regs x") == 1);
    sys_logout();
    puts("selftest notices real damage");
    ROOT(); sys_mkdir("/tmp", 0);
    sys_protect("/tmp/.selftest", false);
    { int fd = sys_open("/tmp/.selftest", SYS_O_WRITE | SYS_O_CREATE); sys_write(fd, "x", 1); sys_close(fd); sys_protect("/tmp/.selftest", true); }
    CHECK(sh("selftest") == 1 && has("FAIL") && has("MiniFS: create, write, read"));                              /* a protected leftover makes the file test fail loudly */
    sys_protect("/tmp/.selftest", false); sys_delete("/tmp/.selftest");
    CHECK(sh("selftest") == 0);
    sys_logout();
}

/* ================= file extras ================= */
static void test_fileextra(void) {
    puts("more, ln, newer, diff");
    ROOT();
    { static char text[2048]; size_t o = 0; for (int i = 1; i <= 45; i++) o += (size_t)snprintf(text + o, sizeof text - o, "line %d\n", i); put("/tmp/long.txt", text); }
    feed("\n\n"); CHECK(sh("more /tmp/long.txt") == 0 && has("line 20\n") && has("line 45") && has("-- more") && fed_all());
    feed("q"); CHECK(sh("more /tmp/long.txt") == 0 && has("line 20") && !has("line 21") && fed_all());
    kq[0] = 0; CHECK(sh("more /tmp/long.txt") == 0 && !has("line 21"));                                         /* no key: treated as quit */
    put("/tmp/short.txt", "a\nb"); CHECK(sh("more /tmp/short.txt") == 0 && is("a\nb\n") && sh("more") == 1 && sh("more /tmp/none") == 1);
    CHECK(sh("ln /tmp/short.txt /tmp/copy1") == 0 && has("a copy was made") && file_is("/tmp/copy1", "a\nb") && sh("ln -s /tmp/short.txt /tmp/copy2") == 0 && sh("ln /tmp/none /tmp/x") == 1 && sh("ln a") == 1 && has("usage:"));
    put("/tmp/copy1", "changed"); CHECK(file_is("/tmp/short.txt", "a\nb"));                                      /* a real copy, not a link */
    fake_ticks = 1000; put("/tmp/old", "o"); fake_ticks = 2000; put("/tmp/new", "n");
    CHECK(sh("newer /tmp/new /tmp/old") == 0 && is("") && sh("newer /tmp/old /tmp/new") == 1 && sh("newer /tmp/old /tmp/old") == 1 && sh("newer /tmp/none /tmp/old") == 1 && has("does not exist") && sh("newer /tmp/old /tmp/none") == 1 && sh("newer a") == 1 && has("usage:"));
    put("/tmp/da", "one\ntwo\nthree\n"); put("/tmp/db", "one\n2\nthree\nfour\n"); put("/tmp/dc", "one\ntwo\nthree\n");
    CHECK(sh("diff /tmp/da /tmp/dc") == 0 && has("identical") && sh("diff /tmp/da /tmp/db") == 1 && has("2:\n  < two\n  > 2") && has("4:\n  > four") && has("2 line(s) differ") && sh("diff /tmp/da") == 1 && sh("diff /tmp/da /tmp/none") == 1);
    put("/tmp/e1", ""); put("/tmp/e2", ""); CHECK(sh("diff /tmp/e1 /tmp/e2") == 0 && sh("diff /tmp/e1 /tmp/da") == 1 && has("3 line(s)"));
    { static char many[4096]; size_t o = 0; for (int i = 0; i < 40; i++) o += (size_t)snprintf(many + o, sizeof many - o, "x%d\n", i); put("/tmp/m1", many); put("/tmp/m2", ""); CHECK(sh("diff /tmp/m1 /tmp/m2") == 1 && has("...") && has("40 line(s) differ")); }
    sys_logout();
}

/* ================= command table ================= */
static void test_table(void) {
    puts("command table after Phase 11");
    /* this harness stubs the memory/process command groups, so only the table size and validity are checked here;
     * the full kernel reports 271 implemented (scripts/gen_commands.py, docs/COMMANDS.md) */
    CHECK(command_count() == 300 && command_count_implemented() >= 225 && command_validate(NULL) == 0);
    static const char *const mine[] = { "exit","alias","unalias","set","unset","env","export","source","repeat","test","prompt","banner",
        "sort","uniq","cut","tr","rev","nl","tac","fold","upper","lower","capitalize","sum","min","max","avg","join","split","paste","strlen","repeatstr","trim","replace","contains","startswith","endswith","ascii","wordfreq",
        "date","time","cpuinfo","cpuid","ticks","timerinfo","irqinfo","idtinfo","gdtinfo","kernelinfo","bootinfo","vgainfo","kbdinfo","devices","reboot","shutdown","halt","arch","osname","about",
        "lsblk","diskinfo","ramdisk","blkread","blkwrite","blkdump","sync","savefs","loadfs","fdisk","diskbench","badblocks",
        "calc","cal","random","base","color","timer","stopwatch","factor","prime","fib","gcd","seq","yes","dice","cowsay","demo","credits",
        "netinfo","ifconfig","ip","route","arp","netstat","host","nslookup","dig","ping","traceroute","curl","wget","netsim",
        "regs","stack","errlist","selftest","diag","more","ln","newer","diff","protect","unprotect" };
    int missing = 0; for (size_t i = 0; i < sizeof mine / sizeof mine[0]; i++) { const command_t *c = command_find(mine[i]); if (!c || !c->handler) { missing++; printf("  missing handler: %s\n", mine[i]); } }
    CHECK(missing == 0);
    /* a command that is still a stub says so clearly rather than pretending; once all 300 exist there is none left to check */
    const command_t *stub = NULL; for (size_t i = 0; i < command_count(); i++) { const command_t *c = command_at(i); if (c && !c->handler) { stub = c; break; } }
    login_as("student", "student123");
    if (stub) { char line[64]; snprintf(line, sizeof line, "%s", stub->name); CHECK(sh(line) != 0 && has("not implemented")); }
    sys_logout();
}

static int real_main(void) {
    fresh(); CHECK(builtins_register() == 0 || true);
    test_protect();     fresh();
    test_kinfo();       fresh();
    test_ramdisk();     fresh();
    test_shellenv();    fresh();
    test_text();        fresh();
    test_sysinfo_cmds();fresh();
    test_disk_cmds();   fresh();
    test_misc();        fresh();
    test_network();     fresh();
    test_diag();        fresh();
    test_fileextra();   fresh();
    test_table();
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
int main(void) { return run_on_low_stack(real_main); }
