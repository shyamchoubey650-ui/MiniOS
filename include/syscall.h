#ifndef SYSCALL_H
#define SYSCALL_H
/* MiniOS system-call interface.
 *
 * Calling convention (i386, vector 0x80):   eax = call number
 *                                           ebx, ecx, edx, esi = arguments 1..4
 *                                           eax = result: >= 0 success value, < 0 error (SYS_E*)
 *
 * REAL:     the int 0x80 trap, a dispatch table, argument validation, per-process
 *           file-descriptor tables and allocation lists (freed when the process ends),
 *           error translation, call statistics.
 * NOT DONE: ring 3. Every task still runs in ring 0, so validation catches wild or accidental
 *           pointers, not a malicious task (there is no memory protection yet). */
#include "include/types.h"
#include "include/filesystem.h"
#include "include/users.h"
#include "include/kinfo.h"
#include "include/ramdisk.h"
#include "include/logger.h"

/* ---- error codes (the first 13 equal the MiniFS FS_ERR_* values on purpose) ---- */
#define SYS_OK              0
#define SYS_ENOENT        (-1)    /* no such file or directory */
#define SYS_EEXIST        (-2)
#define SYS_ENOTDIR       (-3)
#define SYS_EISDIR        (-4)
#define SYS_ENOTEMPTY     (-5)
#define SYS_EINVAL        (-6)    /* bad argument */
#define SYS_ENAMETOOLONG  (-7)
#define SYS_EPATHTOOLONG  (-8)
#define SYS_ENOSPC        (-9)    /* file-system node limit or out of memory */
#define SYS_EFBIG         (-10)
#define SYS_EACCES        (-11)   /* permission denied */
#define SYS_EBUSY         (-12)
#define SYS_EINTOSELF     (-13)
#define SYS_ESTALE        (-14)   /* the file behind a descriptor was removed or replaced */
#define SYS_EBADF         (-15)
#define SYS_EMFILE        (-16)   /* too many open files */
#define SYS_EFAULT        (-17)   /* bad pointer */
#define SYS_ENOSYS        (-18)   /* unknown system call */
#define SYS_ESRCH         (-19)   /* no such process */
#define SYS_ECHILD        (-20)   /* not a child / no children */
#define SYS_EINTR         (-21)   /* interrupted (Ctrl+C) */
#define SYS_ENOMEM        (-22)   /* kernel heap exhausted */
#define SYS_EPERM         (-23)   /* operation not allowed on this process (idle, shell) */
#define SYS_EAGAIN        (-24)   /* process table full */
#define SYS_ELIMIT        (-25)   /* per-process limit reached */
#define SYS_ESTATE        (-26)   /* process not in a state where this applies */
#define SYS_EAUTH         (-27)   /* wrong user name or password */
#define SYS_ELOCKED       (-28)   /* account is locked */
#define SYS_ENOUSER       (-29)   /* no such user / nobody is logged in */
#define SYS_EBLOCK        (-31)   /* bad block number or block device not ready */
#define SYS_EFLOCK        (-32)   /* file locked by another user (equals FS_ERR_LOCKED) */
#define SYS_EPROTECT      (-30)   /* protected file or directory (equals FS_ERR_PROTECTED) */
#define SYS_ERR_MIN       (-32)

const char *sys_strerror(int code);    /* lib/syserr.c */
const char *sys_errname(int code);     /* "ENOENT", ... */

/* ---- call numbers ---- */
enum {
    SYS_EXIT = 0, SYS_GETPID, SYS_GETPPID, SYS_YIELD, SYS_SLEEP,
    SYS_PROCESS_CREATE, SYS_PROCESS_KILL, SYS_PROCESS_WAIT, SYS_PROCESS_INFO, SYS_PROCESS_LIST,
    SYS_PROCESS_SUSPEND, SYS_PROCESS_RESUME, SYS_SETPRIORITY,
    SYS_WRITE, SYS_READ, SYS_OPEN, SYS_CLOSE, SYS_LSEEK,
    SYS_CREATE, SYS_DELETE, SYS_MKDIR, SYS_RMDIR, SYS_RENAME, SYS_STAT, SYS_FSTAT, SYS_READDIR,
    SYS_GETCWD, SYS_CHDIR, SYS_CHMOD, SYS_TOUCH, SYS_TRUNCATE,
    SYS_MEMORY_ALLOC, SYS_MEMORY_FREE,
    SYS_SYSINFO, SYS_TIME, SYS_FDINFO, SYS_STATS,
    /* Phase 10: identity, accounts and ownership */
    SYS_GETUID, SYS_GETCRED, SYS_CHOWN, SYS_UMASK,
    SYS_LOGIN, SYS_LOGOUT, SYS_SU, SYS_SUDO_BEGIN, SYS_SUDO_END, SYS_VERIFY,
    SYS_USER_GET, SYS_USER_LOOKUP, SYS_USER_ADD, SYS_USER_DEL, SYS_USER_PASSWD, SYS_USER_MOD, SYS_AUDIT_GET,
    /* Phase 11: kernel information, power, protection, the RAM-disk */
    SYS_KINFO, SYS_POWER, SYS_PROTECT, SYS_BLK_INFO, SYS_BLK_READ, SYS_BLK_WRITE, SYS_FS_SAVE, SYS_FS_LOAD,
    /* Phase 12: the kernel log */
    SYS_LOG_GET, SYS_LOG_CTL, SYS_LOCK,
    SYS_COUNT
};

/* ---- open flags ---- */
#define SYS_O_READ    1
#define SYS_O_WRITE   2
#define SYS_O_CREATE  4
#define SYS_O_TRUNC   8
#define SYS_O_APPEND  16
#define SYS_O_EXCL    32    /* with SYS_O_CREATE: fail if the file exists */
#define SYS_SEEK_SET  0
#define SYS_SEEK_CUR  1
#define SYS_SEEK_END  2
#define SYS_MKDIR_PARENTS 1

#define SYS_MAX_FDS     16  /* 0,1,2 are stdin/stdout/stderr */
#define SYS_MAX_ALLOCS  48  /* the `alloc` command allows 32 of these; the rest is headroom for commands that load files */

/* ---- structures shared with the kernel ---- */
typedef struct {
    int32_t  pid, parent_pid, state, priority, memory_used, exit_code, wait_kind, wait_pid;
    uint32_t cpu_ticks, start_tick, times_scheduled;
    char     name[32];
} sys_procinfo_t;

typedef struct {
    uint32_t uptime_ticks, ticks_per_sec;
    uint32_t ram_bytes, ram_free_bytes, heap_total, heap_used, heap_free;
    uint32_t processes, context_switches;
    uint32_t fs_nodes, fs_max_nodes, fs_data_bytes;
    uint32_t syscalls_total, syscalls_failed;
} sys_info_t;

typedef struct {
    int32_t  fd;
    uint32_t flags, offset, size;
    bool     stale;
    char     path[FS_PATH_MAX];    /* "console" for 0..2 */
} sys_fdinfo_t;

typedef struct {
    uint32_t total, failed;
    uint32_t calls[SYS_COUNT];
    uint32_t errors[SYS_COUNT];
} sys_stats_t;

const char *sys_name(unsigned number);       /* "open", "write", ... or "?" */

/* ---- kernel side (kernel/syscall/syscall.c) ---- */
void    syscall_init(void);                  /* registers int 0x80 and the process cleanup hook */
int32_t syscall_dispatch(uint32_t number, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4);

/* ---- user side (lib/usyscall.c): what commands call ---- */
int  sys_process_exit(int code);                                   /* does not return on success */
int  sys_getpid(void);
int  sys_getppid(void);
int  sys_yield(void);
int  sys_sleep(uint32_t ms);
int  sys_process_create(const char *program, uint32_t arg, int priority);    /* returns the new pid */
int  sys_process_create_named(const char *program, const char *name, uint32_t arg, int priority);   /* name = what `ps` shows */
int  sys_process_kill(int pid);
int  sys_process_wait(int pid, int *exit_code);                    /* pid -1 = any child; returns the reaped pid */
int  sys_process_info(int pid, sys_procinfo_t *out);
int  sys_process_list(int *pids, uint32_t max);                    /* returns the count; (NULL, 0) just counts */
int  sys_process_suspend(int pid);
int  sys_process_resume(int pid);
int  sys_setpriority(int pid, int priority);
int  sys_write(int fd, const void *buf, size_t count);
int  sys_read(int fd, void *buf, size_t count);
int  sys_open(const char *path, int flags);
int  sys_close(int fd);
int  sys_lseek(int fd, int32_t offset, int whence);
int  sys_create(const char *path);
int  sys_delete(const char *path);
int  sys_mkdir(const char *path, int flags);
int  sys_rmdir(const char *path);
int  sys_rename(const char *from, const char *to);
int  sys_stat(const char *path, fs_stat_t *out);
int  sys_fstat(int fd, fs_stat_t *out);
int  sys_readdir(const char *path, fs_dirent_t *out, uint32_t max);  /* returns entries copied; (NULL, 0) just counts */
int  sys_getcwd(char *buf, size_t size);
int  sys_chdir(const char *path);
int  sys_chmod(const char *path, uint16_t mode);
int  sys_touch(const char *path);
int  sys_truncate(const char *path, uint32_t size);
void *sys_memory_alloc(size_t size);                               /* NULL on failure; use sys_memory_alloc_err() for the code */
int  sys_memory_alloc_err(size_t size, void **out);                /* 0 or a SYS_E* code */
int  sys_memory_free(void *ptr);
int  sys_sysinfo(sys_info_t *out);
uint32_t sys_time(void);                                           /* timer ticks since boot */
int  sys_fdinfo(int fd, sys_fdinfo_t *out);
int  sys_stats(sys_stats_t *out);

/* Phase 10. Strings are copied into the kernel and wiped afterwards (passwords). Admin calls need effective uid 0. */
int  sys_getuid(void);                                             /* real uid; SYS_ENOUSER when logged out */
int  sys_getcred(cred_info_t *out);
int  sys_chown(const char *path, uint32_t uid, uint32_t gid);       /* UID_KEEP leaves a field unchanged; root only */
int  sys_umask(int32_t mask);                                      /* mask < 0 reads; returns the previous mask */
int  sys_login(const char *name, const char *password);
int  sys_logout(void);
int  sys_su(const char *name, const char *password);
int  sys_sudo_begin(const char *password);
int  sys_sudo_end(void);
int  sys_verify(const char *name, const char *password);
int  sys_user_get(uint32_t index, user_info_t *out);
int  sys_user_lookup(const char *name, int32_t uid, user_info_t *out);   /* name NULL = by uid */
int  sys_user_add(const char *name, const char *password, uint32_t flags);   /* returns the new uid */
int  sys_user_del(const char *name, bool remove_home);
int  sys_user_passwd(const char *name, const char *old_pw, const char *new_pw);
int  sys_user_mod(const char *name, int op, const char *value);
int  sys_audit_get(uint32_t index, audit_event_t *out);            /* out NULL: number of events kept */

/* Phase 11. */
int  sys_kinfo(int what, uint32_t index, void *out, uint32_t size);  /* KI_* selector, see include/kinfo.h; returns 0 or SYS_E* */
int  sys_beep(bool on, uint32_t hz);                              /* PC speaker; any user (Phase 12) */
int  sys_power(int op);                                             /* POWER_*; effective uid 0 only; does not return on success */
/* Phase 12: kernel log. which = SYS_LOG_RING or SYS_LOG_BOOT; index 0 = oldest entry you may see; out NULL = how many.
 * Non-root callers do not see entries from the private sources (sec, shell). */
#define SYS_LOG_RING 0
#define SYS_LOG_BOOT 1
int  sys_log_get(int which, uint32_t index, log_entry_t *out);
#define SYS_LOGCTL_STATS      1    /* out = log_stats_t                      */
#define SYS_LOGCTL_SET_LEVEL  2    /* arg = LOG_*            (root)          */
#define SYS_LOGCTL_CLEAR      3    /*                        (root)          */
#define SYS_LOGCTL_TRACE      4    /* arg = 0/1 syscall trace (root)         */
#define SYS_LOGCTL_SCHED      5    /* arg = 0/1 scheduler trace (root)       */
int  sys_log_ctl(int op, uint32_t arg, void *out);
int  sys_lock(const char *path, bool on);       /* advisory lock / unlock a file (Phase 12) */
int  sys_protect(const char *path, bool on);                        /* owner or root */
int  sys_blk_info(ramdisk_info_t *out);
int  sys_blk_read(uint32_t sector, void *buf512);                   /* raw device access: effective uid 0 only */
int  sys_blk_write(uint32_t sector, const void *buf512);
int  sys_fs_save(uint32_t *bytes_out);                              /* MiniFS -> RAM-disk image; returns the node count; root only */
int  sys_fs_load(void);                                             /* RAM-disk image -> MiniFS (replaces everything); returns the node count; root only */
#endif
