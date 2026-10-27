/* Diagnostics (Phase 11): regs stack errlist selftest diag.
 * selftest and diag run REAL checks on the live kernel (heap walk, MiniFS fsck, system calls, SHA-256, the command table).
 * (The log commands live in log_commands.c, Phase 12.) */
#include "include/builtins.h"
#include "include/cmdutil.h"
#include "include/screen.h"
#include "include/syscall.h"
#include "include/heap.h"
#include "include/filesystem.h"
#include "include/sha256.h"
#include "include/mathlib.h"
#include "include/kinfo.h"
#include "include/ramdisk.h"
#include "include/timer.h"
#include "include/numparse.h"
#include "include/string.h"

/* ---------- regs / stack ---------- */
static int cmd_regs(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("regs     (the CPU registers at the moment this command runs, inside the shell task)");
#ifdef MINIOS_HOST_TEST
    return cu_error("regs", "not available in the host test build.");
#else
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, eflags, cs, ds, ss, cr0;
    __asm__ volatile ("mov %%eax,%0; mov %%ebx,%1; mov %%ecx,%2; mov %%edx,%3" : "=m"(eax), "=m"(ebx), "=m"(ecx), "=m"(edx));
    __asm__ volatile ("mov %%esi,%0; mov %%edi,%1; mov %%ebp,%2; mov %%esp,%3" : "=m"(esi), "=m"(edi), "=m"(ebp), "=m"(esp));
    __asm__ volatile ("pushf; pop %0; mov %%cs,%1; mov %%ds,%2; mov %%ss,%3; mov %%cr0,%4" : "=r"(eflags), "=r"(cs), "=r"(ds), "=r"(ss), "=r"(cr0));
    screen_puts("EAX="); screen_put_hex(eax); screen_puts("  EBX="); screen_put_hex(ebx); screen_puts("  ECX="); screen_put_hex(ecx); screen_puts("  EDX="); screen_put_hex(edx);
    screen_puts("\nESI="); screen_put_hex(esi); screen_puts("  EDI="); screen_put_hex(edi); screen_puts("  EBP="); screen_put_hex(ebp); screen_puts("  ESP="); screen_put_hex(esp);
    screen_puts("\nEFLAGS="); screen_put_hex(eflags); screen_puts((eflags & 0x200) ? " (interrupts on)" : " (interrupts off)");
    screen_puts("\nCS="); screen_put_hex(cs & 0xFFFF); screen_puts("  DS="); screen_put_hex(ds & 0xFFFF); screen_puts("  SS="); screen_put_hex(ss & 0xFFFF); screen_puts("  CR0="); screen_put_hex(cr0);
    screen_puts("\n(CS=0x08 means ring 0: this whole kernel, shell included, runs with full privilege)\n");
    return 0;
#endif
}

static int cmd_stack(int argc, const char **argv) {
    uint32_t n = 16;
    if (argc > 2 || (argc == 2 && (!cu_parse_count(argv[1], &n, 64) || n == 0))) return cu_usage("stack [words 1-64]     (the top of this task's stack)");
#ifdef MINIOS_HOST_TEST
    return cu_error("stack", "not available in the host test build.");
#else
    const uint32_t *sp = (const uint32_t *)__builtin_frame_address(0);
    for (uint32_t i = 0; i < n; i++) { screen_put_hex((uint32_t)(uintptr_t)(sp + i)); screen_puts(": "); screen_put_hex(sp[i]); screen_putc(i % 2 ? '\n' : ' '); screen_puts(i % 2 ? "" : "  "); }
    if (n % 2) screen_putc('\n');
    return 0;
#endif
}

/* ---------- errlist ---------- */
static int cmd_errlist(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("errlist");
    screen_puts("CODE  NAME           MEANING\n");
    for (int c = 0; c >= SYS_ERR_MIN; c--) {
        screen_putc(c ? '-' : ' '); screen_put_dec((uint32_t)(c ? -c : 0)); screen_puts(c > -10 ? "    " : "   ");
        cu_pad(sys_errname(c), 14); screen_puts(sys_strerror(c)); screen_putc('\n');
    }
    return 0;
}

/* ---------- selftest ---------- */
typedef struct { uint32_t run, failed; } suite_t;
static void verdict(suite_t *s, const char *name, bool ok, const char *detail) {
    s->run++; if (!ok) s->failed++;
    cu_color(ok ? VGA_LIGHT_GREEN : VGA_LIGHT_RED); screen_puts(ok ? "  PASS  " : "  FAIL  "); cu_color(VGA_LIGHT_GREY);
    screen_puts(name);
    if (!ok && detail) { screen_puts("  ("); screen_puts(detail); screen_putc(')'); }
    screen_putc('\n');
}

static bool test_memory(void) {
    char *a = sys_memory_alloc(100), *b = sys_memory_alloc(2000);
    if (!a || !b) { if (a) sys_memory_free(a); if (b) sys_memory_free(b); return false; }
    for (int i = 0; i < 100; i++) a[i] = (char)i;
    for (int i = 0; i < 2000; i++) b[i] = (char)(i * 7);
    bool ok = a[99] == 99 && b[1999] == (char)(1999 * 7);
    ok = sys_memory_free(a) == 0 && sys_memory_free(b) == 0 && ok;
    return ok && sys_memory_free(a) != 0;                              /* a double free must be refused */
}
static bool test_files(void) {
    const char *p = "/tmp/.selftest"; char buf[16] = { 0 };
    sys_delete(p);
    int fd = sys_open(p, SYS_O_WRITE | SYS_O_CREATE | SYS_O_EXCL);
    if (fd < 0) return false;
    bool ok = sys_write(fd, "selftest", 8) == 8; sys_close(fd);
    fd = sys_open(p, SYS_O_READ);
    ok = ok && fd >= 0 && sys_read(fd, buf, 8) == 8 && !strcmp(buf, "selftest");
    if (fd >= 0) sys_close(fd);
    ok = ok && sys_open(p, SYS_O_WRITE | SYS_O_CREATE | SYS_O_EXCL) == SYS_EEXIST;      /* duplicates are refused */
    return sys_delete(p) == 0 && sys_stat(p, NULL) != 0 && ok;
}
static bool test_syscalls(void) {
    char b[4];
    return sys_getpid() >= 1 && sys_read(99, b, 1) == SYS_EBADF && sys_close(99) == SYS_EBADF && sys_open("", SYS_O_READ) < 0 && sys_getuid() >= 0;
}
static bool test_sha(void) {
    uint8_t d[32]; sha256("abc", 3, d);
    return d[0] == 0xba && d[1] == 0x78 && d[2] == 0x16 && d[3] == 0xbf && d[31] == 0xad;
}
static bool test_math(void) {
    int64_t v;
    return calc_eval("2+3*4", &v) == CALC_OK && v == 14 && calc_eval("1/0", &v) == CALC_ERR_DIV0 && math_gcd(48, 18) == 6 && math_is_prime(97) && !math_is_prime(91);
}
static bool test_users(void) { return sys_verify("nosuchuser", "x") == SYS_ENOUSER && sys_verify("root", "definitely-not-the-password") == SYS_EAUTH; }
static bool test_commands(void) { size_t bad; return command_validate(&bad) == 0 && command_count() == COMMAND_EXPECTED; }
static bool test_heapfs(void) { fs_check_report_t r; return heap_check(NULL) == HEAP_OK && fs_check(&r) == 0; }
static bool test_clock(void) { ki_rtc_t r; ki_timer_t t; return sys_kinfo(KI_RTC, 0, &r, sizeof r) == 0 && r.year >= 2000 && sys_kinfo(KI_TIMER, 0, &t, sizeof t) == 0 && t.hz == TIMER_HZ; }
static bool test_blk(void) { ramdisk_info_t i; return sys_blk_info(&i) == 0 && i.sectors == RAMDISK_SECTORS && i.sector_size == RAMDISK_SECTOR; }

static int cmd_selftest(int argc, const char **argv) {
    (void)argv; suite_t s = { 0, 0 };
    if (argc != 1) return cu_usage("selftest");
    screen_puts("Running checks on the live kernel...\n");
    verdict(&s, "heap: allocate, write, free, double free refused", test_memory(), "memory manager");
    verdict(&s, "MiniFS: create, write, read, duplicate refused, delete", test_files(), "file system or permissions on /tmp");
    verdict(&s, "heap walk and MiniFS fsck are consistent", test_heapfs(), "run 'fsck' for details");
    verdict(&s, "system calls: bad descriptors and bad paths are rejected", test_syscalls(), "syscall layer");
    verdict(&s, "SHA-256 gives the standard digest for \"abc\"", test_sha(), "hash code");
    verdict(&s, "calculator and number theory", test_math(), "mathlib");
    verdict(&s, "accounts: unknown user and wrong password are refused", test_users(), "user manager");
    verdict(&s, "command table: 300 valid, unique entries", test_commands(), "command registry");
    verdict(&s, "clock and timer report sane values", test_clock(), "RTC or PIT");
    verdict(&s, "RAM-disk geometry", test_blk(), "block device");
    cu_color(s.failed ? VGA_LIGHT_RED : VGA_LIGHT_GREEN);
    screen_put_dec(s.run - s.failed); screen_putc('/'); screen_put_dec(s.run); screen_puts(s.failed ? " checks passed, SOME FAILED\n" : " checks passed\n");
    cu_color(VGA_LIGHT_GREY);
    return s.failed ? 1 : 0;
}

/* ---------- diag ---------- */
static void row(const char *name, bool ok) { cu_pad(name, 14); cu_color(ok ? VGA_LIGHT_GREEN : VGA_LIGHT_RED); screen_puts(ok ? "OK    " : "FAULT "); cu_color(VGA_LIGHT_GREY); }

static int cmd_diag(int argc, const char **argv) {
    (void)argv; sys_info_t si; heap_stats_t hs; fs_check_report_t fr; ki_timer_t t; ramdisk_info_t ri; size_t implemented, cat_impl;
    if (argc != 1) return cu_usage("diag");
    (void)cat_impl;
    sys_sysinfo(&si); heap_stats(&hs); sys_kinfo(KI_TIMER, 0, &t, sizeof t); sys_blk_info(&ri);
    implemented = command_count_implemented();
    int fs_problems = fs_check(&fr);
    screen_puts("SUBSYSTEM     STATE DETAIL\n");
    row("memory", heap_check(NULL) == HEAP_OK); screen_put_dec(hs.used_blocks); screen_puts(" blocks in use, heap walk consistent\n");
    row("filesystem", fs_problems == 0); screen_put_dec(fr.nodes_seen); screen_puts(" nodes, "); screen_put_dec((uint32_t)fs_problems); screen_puts(" problems\n");
    row("processes", si.processes >= 1); screen_put_dec(si.processes); screen_puts(" live, "); screen_put_dec(si.context_switches); screen_puts(" context switches\n");
    row("timer", t.ticks > 0); screen_put_dec(t.ticks); screen_puts(" ticks at "); screen_put_dec(t.hz); screen_puts(" Hz\n");
    row("syscalls", si.syscalls_total > 0); screen_put_dec(si.syscalls_total); screen_puts(" made, "); screen_put_dec(si.syscalls_failed); screen_puts(" returned an error (errors are normal: they mean bad input was refused)\n");
    row("commands", command_validate(NULL) == 0); screen_put_dec((uint32_t)implemented); screen_puts(" of "); screen_put_dec((uint32_t)command_count()); screen_puts(" implemented\n");
    row("ram-disk", ri.sectors == RAMDISK_SECTORS); screen_put_dec(ri.sectors); screen_puts(" sectors, "); screen_puts(ri.image_valid ? "MiniFS image saved" : "no saved image"); screen_putc('\n');
    return fs_problems ? 1 : 0;
}

static const struct { const char *name; cmd_handler_t fn; } bindings[] = {
    { "regs", cmd_regs }, { "stack", cmd_stack }, { "errlist", cmd_errlist }, { "selftest", cmd_selftest }, { "diag", cmd_diag },
};

int diag_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
