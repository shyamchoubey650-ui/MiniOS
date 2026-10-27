/* Host-side tests for the system-call layer. They run the REAL dispatcher, the real process
 * manager/scheduler, MiniFS and the heap. Only the register swap, screen, keyboard and timer are
 * stubbed. User-side wrappers (lib/usyscall.c) call syscall_dispatch() directly in this build.
 * Buffers passed to system calls are static: the validators accept only 32-bit RAM addresses. */
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "include/syscall.h"
#include "include/process.h"
#include "include/scheduler.h"
#include "include/memory.h"
#include "include/heap.h"
#include "include/timer.h"
#include "include/keyboard.h"
#include "include/screen.h"
#include "include/interrupts.h"

extern jmp_buf *stub_jump;
extern int stub_switches;

/* ---- stubs ---- */
static char scr[8192]; static size_t scr_len;
void screen_init(void) {} void screen_clear(void) {} void screen_set_color(uint8_t f, uint8_t b) { (void)f; (void)b; } void screen_backspace(void) {}
void screen_putc(char c) { if (scr_len + 1 < sizeof scr) { scr[scr_len++] = c; scr[scr_len] = 0; } }
void screen_puts(const char *s) { while (*s) screen_putc(*s++); }
void screen_put_dec(uint32_t v) { char b[16]; snprintf(b, sizeof b, "%u", v); screen_puts(b); }
void screen_put_hex(uint32_t v) { char b[16]; snprintf(b, sizeof b, "0x%08X", v); screen_puts(b); }
static uint32_t fake_ticks = 1234;
uint32_t timer_ticks(void) { return fake_ticks; }
void timer_sleep_ms(uint32_t ms) { (void)ms; }
static int keys[16]; static int key_n, key_i;
int keyboard_has_key(void) { return key_i < key_n; }
int keyboard_getkey(void) { return key_i < key_n ? keys[key_i++] : 0; }
static int registered_vector = -1; static interrupt_handler_t registered_handler;
void interrupt_register(uint32_t v, interrupt_handler_t h) { registered_vector = (int)v; registered_handler = h; }
const char *exception_name(uint32_t v) { (void)v; return "exception"; }

static int run, failed;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; printf("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
static int blocked_flag;
#define BLOCKS(stmt) do { jmp_buf jb; stub_jump = &jb; if (setjmp(jb) == 0) { stmt; blocked_flag = 0; } else blocked_flag = 1; stub_jump = NULL; } while (0)

static multiboot_mmap_entry_t map_buf[1]; static multiboot_info_t mbi;
static char code_area[4096] __attribute__((aligned(4096)));          /* stands in for the kernel's protected code */
static char buf[4096], buf2[4096];
static fs_stat_t st; static fs_dirent_t dents[64]; static sys_procinfo_t pinfo; static sys_info_t sinfo; static sys_fdinfo_t finfo; static sys_stats_t sstats;
static int pids[PROC_MAX + 4]; static int exit_code;

static void boot(void) {
    map_buf[0] = (multiboot_mmap_entry_t){ 20, 0x100000, 0x4000000, 1 };
    memset(&mbi, 0, sizeof mbi);
    mbi.flags = MULTIBOOT_FLAG_MMAP; mbi.mmap_addr = (uint32_t)(uintptr_t)map_buf; mbi.mmap_length = sizeof map_buf;
    memory_init(&mbi, 0x100000, 0x110000);
    memory_set_code_range((uintptr_t)code_area, (uintptr_t)code_area + sizeof code_area);
    stub_jump = NULL; key_n = key_i = 0; scr_len = 0; scr[0] = 0;
    fs_set_clock(timer_ticks); fs_set_user(FS_UID_ROOT, FS_UID_ROOT); fs_init();
    process_init();
    syscall_init();
}
static size_t used_blocks(void) { heap_stats_t h; heap_stats(&h); return h.used_blocks; }
static void switch_to(int pid) { for (int i = 0; i < 2 * PROC_MAX && process_current_pid() != pid; i++) schedule(); }
static void nop(void *a) { (void)a; }

static void test_dispatch_and_errors(void) {
    puts("dispatch, errors, registration");
    boot();
    CHECK(registered_vector == SYSCALL_VECTOR && registered_handler != NULL);
    CHECK(syscall_dispatch(999, 0, 0, 0, 0) == SYS_ENOSYS && syscall_dispatch(SYS_COUNT, 0, 0, 0, 0) == SYS_ENOSYS && syscall_dispatch(0xFFFFFFFFu, 0, 0, 0, 0) == SYS_ENOSYS);
    CHECK(!strcmp(sys_name(SYS_OPEN), "open") && !strcmp(sys_name(SYS_COUNT), "?") && !strcmp(sys_name(SYS_PROCESS_CREATE), "process_create"));
    for (int i = 0; i < SYS_COUNT; i++) CHECK(strcmp(sys_name((unsigned)i), "?") != 0);          /* every number has a handler */
    for (int c = -1; c >= SYS_ERR_MIN; c--) CHECK(strcmp(sys_strerror(c), "unknown error") != 0 && strlen(sys_strerror(c)) > 3);
    CHECK(!strcmp(sys_strerror(-99), "unknown error") && !strcmp(sys_strerror(0), "success"));
    /* the first 13 syscall errors are numerically the MiniFS errors, so FS codes pass through unchanged */
    CHECK(SYS_ENOENT == FS_ERR_NOT_FOUND && SYS_EEXIST == FS_ERR_EXISTS && SYS_EACCES == FS_ERR_PERMISSION && SYS_EINTOSELF == FS_ERR_INTO_SELF && SYS_EFBIG == FS_ERR_TOO_BIG);
    CHECK(sys_stats(&sstats) == 0 && sstats.total >= 3 && sstats.failed >= 3);
    uint32_t t = sstats.total;
    sys_getpid(); sys_stats(&sstats);
    CHECK(sstats.total == t + 2 && sstats.calls[SYS_GETPID] == 1);
    sys_open("/missing", SYS_O_READ); sys_stats(&sstats);
    CHECK(sstats.errors[SYS_OPEN] == 1 && sstats.calls[SYS_OPEN] == 1);
    CHECK(syscall_dispatch(SYS_STATS, 0, 0, 0, 0) == SYS_EFAULT);

    /* the int 0x80 register convention: eax=number, ebx,ecx,edx,esi=arguments, result back in eax */
    registers_t r; memset(&r, 0, sizeof r);
    r.eax = SYS_GETPID; registered_handler(&r); CHECK(r.eax == 1);
    r.eax = 4000; registered_handler(&r); CHECK((int32_t)r.eax == SYS_ENOSYS);
    strcpy(buf, "/tmp/regtest");
    r.eax = SYS_OPEN; r.ebx = (uint32_t)(uintptr_t)buf; r.ecx = SYS_O_READ | SYS_O_WRITE | SYS_O_CREATE; registered_handler(&r);
    CHECK((int32_t)r.eax == 3 && fs_stat("/tmp/regtest", NULL) == FS_OK);
    strcpy(buf2, "hello");
    r.eax = SYS_WRITE; r.ebx = 3; r.ecx = (uint32_t)(uintptr_t)buf2; r.edx = 5; registered_handler(&r);
    CHECK((int32_t)r.eax == 5);
    r.eax = SYS_LSEEK; r.ebx = 3; r.ecx = (uint32_t)-2; r.edx = SYS_SEEK_END; registered_handler(&r);  /* signed offset travels in a register */
    CHECK((int32_t)r.eax == 3);
    r.eax = SYS_CLOSE; r.ebx = 3; registered_handler(&r); CHECK(r.eax == 0);
}

static void test_pointer_validation(void) {
    puts("pointer validation");
    boot();
    CHECK(sys_stat(NULL, &st) == SYS_EFAULT && sys_stat("/", NULL) == SYS_EFAULT && sys_open(NULL, SYS_O_READ) == SYS_EFAULT);
    CHECK(sys_write(1, NULL, 5) == SYS_EFAULT && sys_write(1, NULL, 0) == 0 && sys_read(3, NULL, 5) == SYS_EBADF);
    CHECK(sys_getcwd(NULL, 10) == SYS_EFAULT && sys_readdir(NULL, dents, 4) == SYS_EFAULT && sys_mkdir(NULL, 0) == SYS_EFAULT);
    CHECK(sys_rename("/tmp", NULL) == SYS_EFAULT && sys_rename(NULL, "/x") == SYS_EFAULT && sys_process_create(NULL, 1, 10) == SYS_EFAULT);
    CHECK(sys_process_info(1, NULL) == SYS_EFAULT && sys_sysinfo(NULL) == SYS_EFAULT && sys_fdinfo(0, NULL) == SYS_EFAULT);
    CHECK(sys_write(1, (const void *)0xFEE00000u, 4) == SYS_EFAULT);                      /* MMIO, not RAM */
    CHECK(sys_write(1, (const void *)0xFFFFFFF0u, 0x100) == SYS_EFAULT);                  /* wraps around 4 GiB */
    CHECK(sys_write(1, buf, 0x7FFFFFFFu) == SYS_EFAULT);                                   /* far larger than RAM allows */
    CHECK(sys_stat((const char *)0xFEE00000u, &st) == SYS_EFAULT && sys_open((const char *)0xFFFFFFFFu, SYS_O_READ) == SYS_EFAULT);

    /* the protected "kernel code" region can be read but never written through a syscall */
    strcpy(code_area, "/etc/motd");
    CHECK(sys_stat(code_area, &st) == 0);                                                  /* reading from it is fine */
    CHECK(sys_stat("/etc/motd", (fs_stat_t *)code_area) == SYS_EFAULT);
    CHECK(sys_getcwd(code_area, 16) == SYS_EFAULT && sys_sysinfo((sys_info_t *)code_area) == SYS_EFAULT);
    int fd = sys_open("/etc/motd", SYS_O_READ);
    CHECK(fd == 3 && sys_read(fd, code_area, 4) == SYS_EFAULT && sys_read(fd, code_area + 4090, 100) == SYS_EFAULT);
    CHECK(sys_read(fd, buf + 4000, 50) >= 0);                                              /* ordinary RAM is fine */
    memset(buf, 'x', sizeof buf);
    CHECK(sys_stat(buf, &st) == SYS_EPATHTOOLONG);                                         /* no NUL within 128 bytes */
    memset(buf, 'y', 40); buf[40] = 0;
    CHECK(sys_process_create(buf, 1, 10) == SYS_ENAMETOOLONG);
    CHECK(sys_write(1, "ok", 2) == 2 && !strcmp(scr, "ok"));
}

static void test_files(void) {
    puts("descriptors and file calls");
    boot();
    CHECK(sys_open("/tmp/a", 0) == SYS_EINVAL && sys_open("/tmp/a", 64 | SYS_O_READ) == SYS_EINVAL && sys_open("/tmp/a", SYS_O_READ | SYS_O_TRUNC) == SYS_EINVAL && sys_open("/tmp/a", SYS_O_READ | SYS_O_APPEND) == SYS_EINVAL);
    CHECK(sys_open("/tmp/a", SYS_O_READ) == SYS_ENOENT && sys_open("/tmp", SYS_O_READ) == SYS_EISDIR && sys_open("/nodir/a", SYS_O_READ | SYS_O_CREATE) == SYS_ENOENT);
    int fd = sys_open("/tmp/a", SYS_O_READ | SYS_O_WRITE | SYS_O_CREATE);
    CHECK(fd == 3 && fs_stat("/tmp/a", NULL) == FS_OK);
    CHECK(sys_open("/tmp/a", SYS_O_READ | SYS_O_CREATE | SYS_O_EXCL) == SYS_EEXIST);
    CHECK(sys_write(fd, "hello world", 11) == 11 && sys_lseek(fd, 0, SYS_SEEK_SET) == 0);
    memset(buf, 0, 32);
    CHECK(sys_read(fd, buf, 5) == 5 && !strcmp(buf, "hello") && sys_read(fd, buf, 100) == 6 && sys_read(fd, buf, 100) == 0);   /* EOF */
    CHECK(sys_lseek(fd, -5, SYS_SEEK_END) == 6 && sys_read(fd, buf, 100) == 5 && !memcmp(buf, "world", 5));
    CHECK(sys_lseek(fd, 3, SYS_SEEK_SET) == 3 && sys_lseek(fd, 2, SYS_SEEK_CUR) == 5 && sys_lseek(fd, -1, SYS_SEEK_SET) == SYS_EINVAL && sys_lseek(fd, -100, SYS_SEEK_CUR) == SYS_EINVAL);
    CHECK(sys_lseek(fd, 0, 9) == SYS_EINVAL && sys_lseek(fd, FS_MAX_FILE_SIZE + 1, SYS_SEEK_SET) == SYS_EINVAL && sys_lseek(fd, FS_MAX_FILE_SIZE, SYS_SEEK_SET) == (int)FS_MAX_FILE_SIZE);
    CHECK(sys_lseek(fd, 15, SYS_SEEK_SET) == 15 && sys_write(fd, "!", 1) == 1);                       /* writing past the end leaves a zero gap */
    size_t n; char tmp[32]; CHECK(fs_read("/tmp/a", tmp, sizeof tmp, 0, &n) == FS_OK && n == 16 && tmp[11] == 0 && tmp[14] == 0 && tmp[15] == '!' && !memcmp(tmp, "hello world", 11));
    CHECK(sys_fstat(fd, &st) == 0 && st.size == 16 && st.type == FS_FILE);

    /* access modes */
    int ro = sys_open("/tmp/a", SYS_O_READ), wo = sys_open("/tmp/a", SYS_O_WRITE);
    CHECK(ro == 4 && wo == 5 && sys_write(ro, "x", 1) == SYS_EBADF && sys_read(wo, buf, 1) == SYS_EBADF);
    sys_close(wo);
    int ap = sys_open("/tmp/a", SYS_O_WRITE | SYS_O_APPEND);
    CHECK(ap == 5 && sys_write(ap, "++", 2) == 2 && fs_read("/tmp/a", tmp, sizeof tmp, 0, &n) == FS_OK && n == 18 && !memcmp(tmp + 16, "++", 2));
    int tr = sys_open("/tmp/a", SYS_O_WRITE | SYS_O_TRUNC);
    CHECK(tr == 6 && fs_stat("/tmp/a", &st) == FS_OK && st.size == 0);
    CHECK(sys_close(tr) == 0 && sys_close(tr) == SYS_EBADF && sys_close(99) == SYS_EBADF && sys_close(-1) == SYS_EBADF && sys_close(0) == SYS_EBADF && sys_close(2) == SYS_EBADF);
    CHECK(sys_open("/tmp/a", SYS_O_READ) == 6);                                                        /* the lowest free number is reused */
    sys_close(4); sys_close(5); sys_close(6); sys_close(3);

    /* descriptor limit */
    int opened = 0, last;
    while ((last = sys_open("/etc/motd", SYS_O_READ)) >= 0) opened++;
    CHECK(opened == SYS_MAX_FDS - 3 && last == SYS_EMFILE);
    CHECK(sys_close(3) == 0 && sys_open("/etc/motd", SYS_O_READ) == 3);
    for (int i = 3; i < SYS_MAX_FDS; i++) sys_close(i);

    /* a descriptor knows WHICH file it opened, not just the name */
    fd = sys_open("/tmp/s", SYS_O_READ | SYS_O_WRITE | SYS_O_CREATE); sys_write(fd, "one", 3);
    sys_lseek(fd, 0, SYS_SEEK_SET);
    CHECK(sys_delete("/tmp/s") == 0 && sys_read(fd, buf, 3) == SYS_ESTALE && sys_write(fd, "x", 1) == SYS_ESTALE && sys_fstat(fd, &st) == SYS_ESTALE && sys_lseek(fd, 0, SYS_SEEK_SET) == SYS_ESTALE);
    CHECK(sys_fdinfo(fd, &finfo) == 0 && finfo.stale && finfo.size == 0 && !strcmp(finfo.path, "/tmp/s"));
    sys_create("/tmp/s");                                                                             /* same name, different file */
    CHECK(sys_read(fd, buf, 3) == SYS_ESTALE);
    CHECK(sys_close(fd) == 0);
    fd = sys_open("/tmp/r1", SYS_O_READ | SYS_O_CREATE); CHECK(sys_rename("/tmp/r1", "/tmp/r2") == 0 && sys_read(fd, buf, 1) == SYS_ESTALE);
    sys_close(fd);

    /* descriptors keep the absolute path even if the working directory changes */
    sys_chdir("/etc");
    fd = sys_open("motd", SYS_O_READ); sys_chdir("/tmp");
    CHECK(fd == 3 && sys_read(fd, buf, 7) == 7 && !memcmp(buf, "Welcome", 7) && sys_fdinfo(fd, &finfo) == 0 && !strcmp(finfo.path, "/etc/motd") && finfo.offset == 7);
    sys_close(fd); sys_chdir("/");

    /* standard descriptors */
    CHECK(sys_fdinfo(0, &finfo) == 0 && !strcmp(finfo.path, "console") && sys_fdinfo(1, &finfo) == 0 && (finfo.flags & SYS_O_WRITE) && sys_fdinfo(7, &finfo) == SYS_EBADF);
    scr_len = 0; CHECK(sys_write(1, "out", 3) == 3 && sys_write(2, "err", 3) == 3 && !strcmp(scr, "outerr"));
    CHECK(sys_write(0, "x", 1) == SYS_EBADF && sys_read(1, buf, 1) == SYS_EBADF && sys_read(2, buf, 1) == SYS_EBADF);
    keys[0] = 'h'; keys[1] = 'i'; keys[2] = KEY_UP; keys[3] = '\n'; keys[4] = 'z'; key_n = 5; key_i = 0;
    memset(buf, 0, 16);
    CHECK(sys_read(0, buf, 10) == 4 && !memcmp(buf, "hi\nz", 4));                                      /* arrow key dropped, keys beyond the first need no waiting */
    key_n = 3; key_i = 0; keys[0] = 'a'; keys[1] = 'b'; keys[2] = 'c';
    CHECK(sys_read(0, buf, 2) == 2 && sys_read(0, buf, 0) == 0 && sys_read(0, buf, 5) == 1 && buf[0] == 'c');

    /* permissions are enforced through the syscall layer */
    fs_write("/tmp/secret", "s", 1, FS_WRITE_TRUNCATE); fs_chmod("/tmp/secret", 0600);
    fs_set_user(FS_UID_STUDENT, FS_UID_STUDENT);
    CHECK(sys_open("/tmp/secret", SYS_O_READ) == SYS_EACCES && sys_open("/etc/motd", SYS_O_WRITE) == SYS_EACCES && sys_open("/etc/new", SYS_O_WRITE | SYS_O_CREATE) == SYS_EACCES);
    CHECK(sys_delete("/etc/motd") == SYS_EACCES && sys_mkdir("/etc/x", 0) == SYS_EACCES && sys_chmod("/etc/motd", 0777) == SYS_EACCES && sys_rename("/etc/motd", "/tmp/m") == SYS_EACCES);
    CHECK(sys_open("/tmp/secret", SYS_O_READ) == SYS_EACCES && sys_stat("/home/admin/x", &st) == SYS_EACCES && sys_chdir("/home/admin") == SYS_EACCES);
    fs_set_user(FS_UID_ROOT, FS_UID_ROOT);
}

static void test_fs_calls(void) {
    puts("file-system calls");
    boot();
    CHECK(sys_create("/tmp/c") == 0 && sys_create("/tmp/c") == SYS_EEXIST && sys_delete("/tmp/c") == 0 && sys_delete("/tmp/c") == SYS_ENOENT && sys_delete("/tmp") == SYS_EISDIR);
    CHECK(sys_mkdir("/tmp/d1", 0) == 0 && sys_mkdir("/tmp/d1", 0) == SYS_EEXIST && sys_mkdir("/tmp/p/q/r", 0) == SYS_ENOENT && sys_mkdir("/tmp/p/q/r", SYS_MKDIR_PARENTS) == 0 && fs_stat("/tmp/p/q/r", NULL) == FS_OK);
    CHECK(sys_rmdir("/tmp/p") == SYS_ENOTEMPTY && sys_rmdir("/tmp/p/q/r") == 0 && sys_rmdir("/etc/motd") == SYS_ENOTDIR && sys_rmdir("/") == SYS_EBUSY);
    CHECK(sys_rename("/tmp/d1", "/tmp/d2") == 0 && sys_rename("/tmp/d2", "/tmp/d2/in") == SYS_EINTOSELF && sys_rename("/tmp/zz", "/tmp/y") == SYS_ENOENT);
    CHECK(sys_touch("/tmp/t") == 0 && sys_truncate("/tmp/t", 100) == 0 && sys_stat("/tmp/t", &st) == 0 && st.size == 100 && sys_truncate("/tmp/t", FS_MAX_FILE_SIZE + 1) == SYS_EFBIG && sys_truncate("/tmp", 1) == SYS_EISDIR);
    CHECK(sys_chmod("/tmp/t", 0600) == 0 && sys_stat("/tmp/t", &st) == 0 && st.mode == 0600 && sys_chmod("/tmp/t", 01000) == SYS_EINVAL && syscall_dispatch(SYS_CHMOD, (uint32_t)(uintptr_t)"/tmp/t", 0x10000u, 0, 0) == SYS_EINVAL && sys_chmod("/tmp/nothing", 0644) == SYS_ENOENT);
    CHECK(sys_stat("/nowhere", &st) == SYS_ENOENT && sys_stat("/etc/motd/x", &st) == SYS_ENOTDIR && sys_stat("/etc", &st) == 0 && st.type == FS_DIR && !strcmp(st.name, "etc"));
    CHECK(sys_getcwd(buf, sizeof buf) == 1 && !strcmp(buf, "/") && sys_chdir("/etc") == 0 && sys_getcwd(buf, sizeof buf) == 4 && !strcmp(buf, "/etc"));
    CHECK(sys_getcwd(buf, 4) == SYS_EINVAL && sys_getcwd(buf, 5) == 4 && sys_getcwd(buf, 0) == SYS_EINVAL);        /* needs room for the NUL */
    CHECK(sys_chdir("/nowhere") == SYS_ENOENT && sys_chdir("/etc/motd") == SYS_ENOTDIR && sys_chdir("..") == 0 && sys_chdir("") == SYS_EINVAL);
    memset(buf, 'a', 127); buf[127] = 0; strcat(buf2, "");
    CHECK(sys_stat(buf, &st) == SYS_ENAMETOOLONG || sys_stat(buf, &st) == SYS_ENOENT);                          /* 127-char relative name: component limit */

    /* readdir: count, copy, truncation, errors */
    CHECK(sys_readdir("/", NULL, 0) == 6 && sys_readdir("/", dents, 64) == 6 && !strcmp(dents[0].name, "bin") && !strcmp(dents[2].name, "home") && dents[2].type == FS_DIR);
    CHECK(sys_readdir("/", dents, 2) == 2 && !strcmp(dents[1].name, "etc") && sys_readdir("/", dents, 0) == 0 && sys_readdir("/etc", dents, 64) == 2);
    CHECK(sys_readdir("/etc/motd", dents, 4) == SYS_ENOTDIR && sys_readdir("/nowhere", dents, 4) == SYS_ENOENT && sys_readdir("/", (fs_dirent_t *)code_area, 4) == SYS_EFAULT);
    fs_set_user(FS_UID_STUDENT, FS_UID_STUDENT);
    CHECK(sys_readdir("/home/admin", dents, 4) == SYS_EACCES);
    fs_set_user(FS_UID_ROOT, FS_UID_ROOT);

    /* system information */
    fake_ticks = 4242;
    CHECK(sys_time() == 4242 && sys_sysinfo(&sinfo) == 0 && sinfo.uptime_ticks == 4242 && sinfo.ticks_per_sec == TIMER_HZ);
    CHECK(sinfo.ram_bytes > sinfo.ram_free_bytes && sinfo.heap_total == sinfo.heap_used + sinfo.heap_free && sinfo.processes == 2 && sinfo.fs_nodes == sinfo.fs_nodes && sinfo.fs_max_nodes == FS_MAX_NODES);
    CHECK(sinfo.syscalls_total > 0 && sinfo.syscalls_failed > 0 && sinfo.syscalls_failed < sinfo.syscalls_total);
}

static void test_memory(void) {
    puts("memory calls");
    boot();
    size_t base = used_blocks();
    void *p = NULL;
    CHECK(sys_memory_alloc_err(100, &p) == 0 && p != NULL && ((uintptr_t)p & 15) == 0 && (uintptr_t)p >= 0x100000);
    memset(p, 0x5A, 100);
    CHECK(sys_stat("/etc/motd", (fs_stat_t *)p) == 0);                                  /* syscalls may write into the process's own allocation */
    CHECK(sys_memory_free(p) == 0 && sys_memory_free(p) == SYS_EINVAL && sys_memory_free(NULL) == SYS_EINVAL && sys_memory_free((void *)0x1234) == SYS_EINVAL);
    CHECK(sys_memory_alloc(0) == NULL && sys_memory_alloc_err(0, &p) == SYS_EINVAL && sys_memory_alloc_err(0x7FFFFFF0u, &p) == SYS_ENOMEM && p == NULL);
    CHECK(used_blocks() == base + 1);                                                  /* +1: the process's resource block, created on first use */
    void *a[SYS_MAX_ALLOCS + 1]; int n = 0;
    for (; n < SYS_MAX_ALLOCS; n++) { a[n] = sys_memory_alloc(64); if (!a[n]) break; }
    CHECK(n == SYS_MAX_ALLOCS && sys_memory_alloc_err(64, &p) == SYS_ELIMIT);          /* per-process limit */
    CHECK(sys_memory_free(a[0]) == 0 && sys_memory_alloc(64) != NULL);
    for (int i = 1; i < n; i++) sys_memory_free(a[i]);
    size_t after_shell = used_blocks();
    CHECK(after_shell == base + 2);                                                    /* resource block + one allocation */

    /* allocations belong to the process that made them, and die with it */
    void *mine = sys_memory_alloc(256);
    int child = -1; process_create("child", nop, NULL, 10, &child);
    size_t before_child = used_blocks();
    switch_to(child);
    CHECK(sys_memory_free(mine) == SYS_EINVAL);                                         /* not the child's */
    void *c1 = sys_memory_alloc(1000), *c2 = sys_memory_alloc(2000);
    int cfd = sys_open("/tmp/childfile", SYS_O_WRITE | SYS_O_CREATE);
    CHECK(c1 && c2 && cfd == 3 && used_blocks() > before_child + 2);
    switch_to(PID_SHELL);
    CHECK(sys_fdinfo(3, &finfo) == SYS_EBADF);                                          /* the shell never opened fd 3: no sharing */
    CHECK(process_kill(child) == 0);
    CHECK(used_blocks() == before_child);                                               /* stack (-1) and all child allocations returned; +1 is the file it created, which rightly persists */
    CHECK(fs_stat("/tmp/childfile", NULL) == FS_OK);
    CHECK(sys_memory_free(mine) == 0 && heap_check(NULL) == HEAP_OK);
}

static void test_processes(void) {
    puts("process calls");
    boot();
    CHECK(sys_getpid() == 1 && sys_getppid() == 0 && sys_yield() == 0 && sys_process_exit(3) == SYS_EPERM);
    CHECK(sys_sleep(3600001u) == SYS_EINVAL && sys_sleep(0) == 0);
    CHECK(sys_process_create("nothing", 1, 10) == SYS_ENOENT && sys_process_create("demo", 1, 0) == SYS_EINVAL && sys_process_create("demo", 1, 21) == SYS_EINVAL);
    CHECK(sys_process_create("demo", 601, 10) == SYS_EINVAL && sys_process_create("spin", 99999, 10) > 1);                 /* spin takes no argument: ignored */
    int pid = sys_process_create("sleeper", 0, 7);                                                                           /* 0 = the program's default */
    CHECK(pid >= 2 && sys_process_info(pid, &pinfo) == 0 && !strcmp(pinfo.name, "sleeper") && pinfo.priority == 7 && pinfo.parent_pid == 1 && pinfo.state == PROC_READY && pinfo.memory_used == (int)PROC_STACK_SIZE);
    CHECK(sys_process_info(999, &pinfo) == SYS_ESRCH && sys_process_info(-1, &pinfo) == SYS_ESRCH);

    /* listing */
    CHECK(sys_process_list(NULL, 0) == 4);
    int cnt = sys_process_list(pids, PROC_MAX + 4);
    CHECK(cnt == 4 && pids[0] == 0 && pids[1] == 1 && pids[2] < pids[3]);
    CHECK(sys_process_list(pids, 2) == 2 && pids[1] == 1 && sys_process_list(pids, 0) == 0);

    /* control */
    CHECK(sys_process_suspend(pid) == 0 && sys_process_info(pid, &pinfo) == 0 && pinfo.state == PROC_WAITING && sys_process_suspend(pid) == SYS_ESTATE);
    CHECK(sys_process_resume(pid) == 0 && sys_process_resume(pid) == SYS_ESTATE && sys_process_suspend(1) == SYS_EPERM && sys_process_suspend(999) == SYS_ESRCH && sys_process_resume(999) == SYS_ESRCH);
    CHECK(sys_setpriority(pid, 3) == 0 && sys_setpriority(pid, 0) == SYS_EINVAL && sys_setpriority(pid, 21) == SYS_EINVAL && sys_setpriority(0, 5) == SYS_EPERM && sys_setpriority(999, 5) == SYS_ESRCH);
    CHECK(sys_process_kill(999) == SYS_ESRCH && sys_process_kill(0) == SYS_EPERM && sys_process_kill(1) == SYS_EPERM);

    /* wait */
    CHECK(sys_process_wait(999, &exit_code) == SYS_ESRCH && sys_process_wait(1, &exit_code) == SYS_EINVAL);
    CHECK(sys_process_wait(pid, (int *)code_area) == SYS_EFAULT);                                                            /* bad pointer is rejected BEFORE anything is reaped */
    CHECK(sys_process_kill(pid) == 0 && sys_process_kill(pid) == SYS_ESTATE && sys_process_info(pid, &pinfo) == 0 && pinfo.state == PROC_TERMINATED && pinfo.exit_code == PROC_EXIT_KILLED);
    exit_code = 0; CHECK(sys_process_wait(pid, &exit_code) == pid && exit_code == PROC_EXIT_KILLED && sys_process_info(pid, &pinfo) == SYS_ESRCH);
    CHECK(sys_process_wait(pid, &exit_code) == SYS_ESRCH);
    while (sys_process_list(NULL, 0) > 2) { sys_process_list(pids, PROC_MAX); sys_process_kill(pids[2]); sys_process_wait(pids[2], NULL); }
    CHECK(sys_process_wait(-1, &exit_code) == SYS_ECHILD);

    /* blocking calls really block, and results arrive when the process resumes */
    int w = sys_process_create("demo", 1, 10);
    BLOCKS(sys_process_wait(w, &exit_code));
    CHECK(blocked_flag == 1 && process_get(1)->state == PROC_WAITING && process_current_pid() == w);
    BLOCKS(sys_sleep(100));
    CHECK(blocked_flag == 1 && process_get(w)->state == PROC_WAITING && process_get(w)->wait_kind == WAIT_SLEEP && process_get(w)->wake_tick == sched_now() + 10);
    sched_stats_t ss; sched_stats(&ss);
    for (int i = 0; i < 10; i++) { sched_tick(); sched_on_irq_exit(); }
    CHECK(process_current_pid() == w);                                                                                        /* the sleeper woke and was dispatched */
    int cfd = sys_open("/tmp/onexit", SYS_O_WRITE | SYS_O_CREATE); size_t blocks = used_blocks();
    BLOCKS(sys_process_exit(9));                                                                                              /* a process ending itself */
    CHECK(blocked_flag == 1 && process_get(w)->exit_code == 9 && cfd == 3 && process_current_pid() == PID_SHELL);
    sched_finish_switch();
    CHECK(used_blocks() < blocks);
    exit_code = 0; CHECK(sys_process_wait(w, &exit_code) == w && exit_code == 9);

    /* a process can be created under a different display name */
    boot();
    int named = sys_process_create_named("demo", "custom-name", 1, 10);
    CHECK(named >= 2 && sys_process_info(named, &pinfo) == 0 && !strcmp(pinfo.name, "custom-name"));
    CHECK(sys_process_create_named("demo", "0123456789012345678901234567890123456789", 1, 10) == SYS_ENAMETOOLONG && sys_process_create_named("nothing", "x", 1, 10) == SYS_ENOENT);
    int plain = sys_process_create_named("sleeper", NULL, 0, 10);
    CHECK(plain >= 2 && sys_process_info(plain, &pinfo) == 0 && !strcmp(pinfo.name, "sleeper"));
    CHECK(!strcmp(sys_errname(SYS_EBADF), "EBADF") && !strcmp(sys_errname(SYS_OK), "OK") && !strcmp(sys_errname(-77), "?"));

    /* process table full */
    int made = 0, rc;
    while ((rc = sys_process_create("spin", 0, 10)) > 1) made++;
    CHECK(made == PROC_MAX - 2 - 2 && rc == SYS_EAGAIN);                              /* minus the two named test processes still alive */
}

int main(void) {
    test_dispatch_and_errors(); test_pointer_validation(); test_files(); test_fs_calls(); test_memory(); test_processes();
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
