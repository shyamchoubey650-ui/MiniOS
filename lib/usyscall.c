/* User side of the system-call interface: what commands call. Each function packs its arguments into
 * registers and executes `int 0x80` (the kernel build). The host-side tests have no trap gate, so
 * there the same call goes straight to syscall_dispatch(); everything after that point is identical. */
#include "include/syscall.h"

static inline int32_t trap(uint32_t n, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
#ifdef MINIOS_HOST_TEST
    return syscall_dispatch(n, a, b, c, d);
#else
    int32_t r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory", "cc");
    return r;
#endif
}
#define P(x) ((uint32_t)(uintptr_t)(x))

int  sys_process_exit(int code)                  { return trap(SYS_EXIT, (uint32_t)code, 0, 0, 0); }
int  sys_getpid(void)                            { return trap(SYS_GETPID, 0, 0, 0, 0); }
int  sys_getppid(void)                           { return trap(SYS_GETPPID, 0, 0, 0, 0); }
int  sys_yield(void)                             { return trap(SYS_YIELD, 0, 0, 0, 0); }
int  sys_sleep(uint32_t ms)                      { return trap(SYS_SLEEP, ms, 0, 0, 0); }
int  sys_process_create(const char *program, uint32_t arg, int priority) { return trap(SYS_PROCESS_CREATE, P(program), arg, (uint32_t)priority, 0); }
int  sys_process_create_named(const char *program, const char *name, uint32_t arg, int priority) { return trap(SYS_PROCESS_CREATE, P(program), arg, (uint32_t)priority, P(name)); }
int  sys_process_kill(int pid)                   { return trap(SYS_PROCESS_KILL, (uint32_t)pid, 0, 0, 0); }
int  sys_process_wait(int pid, int *code)        { return trap(SYS_PROCESS_WAIT, (uint32_t)pid, P(code), 0, 0); }
int  sys_process_info(int pid, sys_procinfo_t *o){ return trap(SYS_PROCESS_INFO, (uint32_t)pid, P(o), 0, 0); }
int  sys_process_list(int *pids, uint32_t max)   { return trap(SYS_PROCESS_LIST, P(pids), max, 0, 0); }
int  sys_process_suspend(int pid)                { return trap(SYS_PROCESS_SUSPEND, (uint32_t)pid, 0, 0, 0); }
int  sys_process_resume(int pid)                 { return trap(SYS_PROCESS_RESUME, (uint32_t)pid, 0, 0, 0); }
int  sys_setpriority(int pid, int prio)          { return trap(SYS_SETPRIORITY, (uint32_t)pid, (uint32_t)prio, 0, 0); }
int  sys_write(int fd, const void *buf, size_t n){ return trap(SYS_WRITE, (uint32_t)fd, P(buf), (uint32_t)n, 0); }
int  sys_read(int fd, void *buf, size_t n)       { return trap(SYS_READ, (uint32_t)fd, P(buf), (uint32_t)n, 0); }
int  sys_open(const char *path, int flags)       { return trap(SYS_OPEN, P(path), (uint32_t)flags, 0, 0); }
int  sys_close(int fd)                           { return trap(SYS_CLOSE, (uint32_t)fd, 0, 0, 0); }
int  sys_lseek(int fd, int32_t off, int whence)  { return trap(SYS_LSEEK, (uint32_t)fd, (uint32_t)off, (uint32_t)whence, 0); }
int  sys_create(const char *path)                { return trap(SYS_CREATE, P(path), 0, 0, 0); }
int  sys_delete(const char *path)                { return trap(SYS_DELETE, P(path), 0, 0, 0); }
int  sys_mkdir(const char *path, int flags)      { return trap(SYS_MKDIR, P(path), (uint32_t)flags, 0, 0); }
int  sys_rmdir(const char *path)                 { return trap(SYS_RMDIR, P(path), 0, 0, 0); }
int  sys_rename(const char *from, const char *to){ return trap(SYS_RENAME, P(from), P(to), 0, 0); }
int  sys_stat(const char *path, fs_stat_t *o)    { return trap(SYS_STAT, P(path), P(o), 0, 0); }
int  sys_fstat(int fd, fs_stat_t *o)             { return trap(SYS_FSTAT, (uint32_t)fd, P(o), 0, 0); }
int  sys_readdir(const char *path, fs_dirent_t *o, uint32_t max) { return trap(SYS_READDIR, P(path), P(o), max, 0); }
int  sys_getcwd(char *buf, size_t size)          { return trap(SYS_GETCWD, P(buf), (uint32_t)size, 0, 0); }
int  sys_chdir(const char *path)                 { return trap(SYS_CHDIR, P(path), 0, 0, 0); }
int  sys_chmod(const char *path, uint16_t mode)  { return trap(SYS_CHMOD, P(path), mode, 0, 0); }
int  sys_touch(const char *path)                 { return trap(SYS_TOUCH, P(path), 0, 0, 0); }
int  sys_truncate(const char *path, uint32_t sz) { return trap(SYS_TRUNCATE, P(path), sz, 0, 0); }
int  sys_memory_alloc_err(size_t size, void **out) {
    int32_t r = trap(SYS_MEMORY_ALLOC, (uint32_t)size, 0, 0, 0);
    if (r < 0) { *out = NULL; return r; }
    *out = (void *)(uintptr_t)(uint32_t)r;
    return 0;
}
void *sys_memory_alloc(size_t size)              { void *p; return sys_memory_alloc_err(size, &p) == 0 ? p : NULL; }
int  sys_memory_free(void *ptr)                  { return trap(SYS_MEMORY_FREE, P(ptr), 0, 0, 0); }
int  sys_sysinfo(sys_info_t *o)                  { return trap(SYS_SYSINFO, P(o), 0, 0, 0); }
uint32_t sys_time(void)                          { return (uint32_t)trap(SYS_TIME, 0, 0, 0, 0); }
int  sys_fdinfo(int fd, sys_fdinfo_t *o)         { return trap(SYS_FDINFO, (uint32_t)fd, P(o), 0, 0); }
int  sys_stats(sys_stats_t *o)                   { return trap(SYS_STATS, P(o), 0, 0, 0); }

/* ---- Phase 10 ---- */
int  sys_getuid(void)                            { return trap(SYS_GETUID, 0, 0, 0, 0); }
int  sys_getcred(cred_info_t *o)                 { return trap(SYS_GETCRED, P(o), 0, 0, 0); }
int  sys_chown(const char *p, uint32_t u, uint32_t g) { return trap(SYS_CHOWN, P(p), u, g, 0); }
int  sys_umask(int32_t m)                        { return trap(SYS_UMASK, (uint32_t)m, 0, 0, 0); }
int  sys_login(const char *n, const char *pw)    { return trap(SYS_LOGIN, P(n), P(pw), 0, 0); }
int  sys_logout(void)                            { return trap(SYS_LOGOUT, 0, 0, 0, 0); }
int  sys_su(const char *n, const char *pw)       { return trap(SYS_SU, P(n), P(pw), 0, 0); }
int  sys_sudo_begin(const char *pw)              { return trap(SYS_SUDO_BEGIN, P(pw), 0, 0, 0); }
int  sys_sudo_end(void)                          { return trap(SYS_SUDO_END, 0, 0, 0, 0); }
int  sys_verify(const char *n, const char *pw)   { return trap(SYS_VERIFY, P(n), P(pw), 0, 0); }
int  sys_user_get(uint32_t i, user_info_t *o)    { return trap(SYS_USER_GET, i, P(o), 0, 0); }
int  sys_user_lookup(const char *n, int32_t uid, user_info_t *o) { return trap(SYS_USER_LOOKUP, P(n), (uint32_t)uid, P(o), 0); }
int  sys_user_add(const char *n, const char *pw, uint32_t flags) { return trap(SYS_USER_ADD, P(n), P(pw), flags, 0); }
int  sys_user_del(const char *n, bool rm_home)   { return trap(SYS_USER_DEL, P(n), rm_home ? 1u : 0u, 0, 0); }
int  sys_user_passwd(const char *n, const char *o, const char *nw) { return trap(SYS_USER_PASSWD, P(n), P(o), P(nw), 0); }
int  sys_user_mod(const char *n, int op, const char *v) { return trap(SYS_USER_MOD, P(n), (uint32_t)op, P(v), 0); }
int  sys_audit_get(uint32_t i, audit_event_t *o) { return trap(SYS_AUDIT_GET, i, P(o), 0, 0); }

/* ---- Phase 11 ---- */
int  sys_kinfo(int what, uint32_t i, void *o, uint32_t n) { return trap(SYS_KINFO, (uint32_t)what, i, P(o), n); }
int  sys_power(int op)                           { return trap(SYS_POWER, (uint32_t)op, 0, 0, 0); }
int  sys_beep(bool on, uint32_t hz)             { return trap(SYS_POWER, on ? POWER_BEEP_ON : POWER_BEEP_OFF, on ? hz : 0, 0, 0); }
int  sys_log_get(int w, uint32_t i, log_entry_t *o) { return trap(SYS_LOG_GET, (uint32_t)w, i, P(o), 0); }
int  sys_log_ctl(int op, uint32_t arg, void *o)  { return trap(SYS_LOG_CTL, (uint32_t)op, arg, P(o), 0); }
int  sys_lock(const char *p, bool on)            { return trap(SYS_LOCK, P(p), on ? 1u : 0u, 0, 0); }
int  sys_protect(const char *p, bool on)         { return trap(SYS_PROTECT, P(p), on ? 1u : 0u, 0, 0); }
int  sys_blk_info(ramdisk_info_t *o)             { return trap(SYS_BLK_INFO, P(o), 0, 0, 0); }
int  sys_blk_read(uint32_t s, void *b)           { return trap(SYS_BLK_READ, s, P(b), 0, 0); }
int  sys_blk_write(uint32_t s, const void *b)    { return trap(SYS_BLK_WRITE, s, P(b), 0, 0); }
int  sys_fs_save(uint32_t *bytes)                { return trap(SYS_FS_SAVE, P(bytes), 0, 0, 0); }
int  sys_fs_load(void)                           { return trap(SYS_FS_LOAD, 0, 0, 0, 0); }
