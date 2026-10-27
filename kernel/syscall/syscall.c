/* System-call layer: int 0x80 entry, dispatch table, argument validation, per-process
 * file descriptors and allocation tracking.
 *
 * Every pointer that comes from the caller is validated before the kernel touches it
 * (non-NULL, inside RAM, writes never into the kernel's code/read-only data, strings
 * bounded). Every failure returns a negative SYS_E* code; nothing is silently ignored.
 * Per-process resources live in a kmalloc'd block hanging off the PCB and are released by
 * the process-exit hook, so a killed or crashed process cannot leak descriptors or memory. */
#include "include/syscall.h"
#include "include/process.h"
#include "include/scheduler.h"
#include "include/workloads.h"
#include "include/interrupts.h"
#include "include/memory.h"
#include "include/heap.h"
#include "include/timer.h"
#include "include/screen.h"
#include "include/keyboard.h"
#include "include/irqflags.h"
#include "include/logger.h"
#include "include/string.h"

/* ---------- per-process resources ---------- */
typedef struct { bool used; uint32_t flags, offset, ino; char path[FS_PATH_MAX]; } fd_entry_t;
typedef struct { void *ptr; size_t size; } alloc_entry_t;
typedef struct { fd_entry_t fds[SYS_MAX_FDS - 3]; alloc_entry_t allocs[SYS_MAX_ALLOCS]; } proc_res_t;

static sys_stats_t stats;

static proc_res_t *res_get(bool create) {
    PCB *me = process_current();
    if (!me->res && create) me->res = kcalloc(1, sizeof(proc_res_t));
    return me->res;
}

static void syscall_cleanup(PCB *p) {                    /* process exit hook */
    proc_res_t *r = p->res;
    if (!r) return;
    for (int i = 0; i < SYS_MAX_ALLOCS; i++) if (r->allocs[i].ptr) kfree(r->allocs[i].ptr);
    p->res = NULL;
    kfree(r);
}

/* ---------- error translation ---------- */
static int32_t proc_err(int rc) {
    switch (rc) {
        case PROC_OK:              return 0;
        case PROC_ERR_ARG:         return SYS_EINVAL;
        case PROC_ERR_NO_SLOT:     return SYS_EAGAIN;
        case PROC_ERR_NO_MEMORY:   return SYS_ENOMEM;
        case PROC_ERR_NO_SUCH_PID: return SYS_ESRCH;
        case PROC_ERR_PROTECTED:   return SYS_EPERM;
        case PROC_ERR_NOT_CHILD:
        case PROC_ERR_NO_CHILD:    return SYS_ECHILD;
        case PROC_ERR_INTERRUPTED: return SYS_EINTR;
        case PROC_ERR_BAD_STATE:   return SYS_ESTATE;
        case PROC_ERR_SELF_WAIT:   return SYS_EINVAL;
        default:                   return SYS_EINVAL;
    }
}

/* ---------- validated access to caller memory ---------- */
static bool ok_read(uint32_t p, uint32_t n)  { return n == 0 || (p != 0 && n <= 0xFFFFFFFFu - p && memory_range_readable(p, n)); }
static bool ok_write(uint32_t p, uint32_t n) { return n == 0 || (p != 0 && n <= 0xFFFFFFFFu - p && memory_range_writable(p, n)); }

static int32_t copy_out(uint32_t user, const void *src, uint32_t n) {
    if (!ok_write(user, n)) return SYS_EFAULT;
    memcpy((void *)(uintptr_t)user, src, n);
    return 0;
}

/* Copies a NUL-terminated string of at most cap-1 characters. */
static int32_t copy_str(char *dst, size_t cap, uint32_t user) {
    if (user == 0) return SYS_EFAULT;
    for (size_t i = 0; i < cap; i++) {
        if (user + i < user || !memory_range_readable(user + (uint32_t)i, 1)) return SYS_EFAULT;
        char c = *(const volatile char *)(uintptr_t)(user + i);
        dst[i] = c;
        if (c == '\0') return 0;
    }
    dst[cap - 1] = '\0';
    return SYS_EPATHTOOLONG;
}

/* ---------- process calls ---------- */
static int32_t h_exit(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    (void)b; (void)c; (void)d;
    if (process_current_pid() <= PID_SHELL) return SYS_EPERM;      /* idle and the shell never exit */
    process_exit((int)a);
}
static int32_t h_getpid(uint32_t a, uint32_t b, uint32_t c, uint32_t d)  { (void)a; (void)b; (void)c; (void)d; return process_current_pid(); }
static int32_t h_getppid(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { (void)a; (void)b; (void)c; (void)d; return process_current()->parent_pid; }
static int32_t h_yield(uint32_t a, uint32_t b, uint32_t c, uint32_t d)   { (void)a; (void)b; (void)c; (void)d; process_yield(); return 0; }

static int32_t h_sleep(uint32_t ms, uint32_t b, uint32_t c, uint32_t d) {
    (void)b; (void)c; (void)d;
    if (ms > 3600000u) return SYS_EINVAL;                          /* one hour at most */
    uint32_t ticks = (ms * TIMER_HZ + 999) / 1000;
    if (ticks == 0) { process_yield(); return 0; }
    process_sleep_ticks(ticks);
    return 0;
}

static int32_t h_process_create(uint32_t name_ptr, uint32_t arg, uint32_t prio, uint32_t name_override) {
    char name[32], shown[32];
    int32_t rc = copy_str(name, sizeof name, name_ptr);
    if (rc == SYS_EPATHTOOLONG) return SYS_ENAMETOOLONG;
    if (rc) return rc;
    const workload_t *w = workload_find(name);
    if (!w) return SYS_ENOENT;                                     /* no such program */
    int p = (int)prio;
    if (p < PRIORITY_MIN || p > PRIORITY_MAX) return SYS_EINVAL;
    if (arg == 0) arg = w->default_arg;
    if (w->arg_help && (arg < 1 || arg > 600)) return SYS_EINVAL;
    int pid;
    const char *show = NULL;
    if (name_override) {                                           /* optional: the name `ps` displays */
        rc = copy_str(shown, sizeof shown, name_override);
        if (rc == SYS_EPATHTOOLONG) return SYS_ENAMETOOLONG;
        if (rc) return rc;
        show = shown;
    }
    rc = proc_err(workload_spawn(w, show, arg, p, &pid));
    return rc ? rc : pid;
}

static int32_t h_process_kill(uint32_t pid, uint32_t b, uint32_t c, uint32_t d) { (void)b; (void)c; (void)d; return proc_err(process_kill((int)pid)); }

static int32_t h_process_wait(uint32_t pid, uint32_t code_ptr, uint32_t c, uint32_t d) {
    (void)c; (void)d;
    if (code_ptr && !ok_write(code_ptr, sizeof(int))) return SYS_EFAULT;
    int code = 0, rc = process_wait((int)pid, &code);
    if (rc < 0) return proc_err(rc);
    if (code_ptr) copy_out(code_ptr, &code, sizeof code);
    return rc;                                                     /* the reaped pid */
}

static void fill_info(const PCB *p, sys_procinfo_t *o) {
    memset(o, 0, sizeof *o);
    o->pid = p->pid; o->parent_pid = p->parent_pid; o->state = p->state; o->priority = p->priority;
    o->memory_used = p->memory_used; o->exit_code = p->exit_code; o->wait_kind = p->wait_kind; o->wait_pid = p->wait_pid;
    o->cpu_ticks = p->cpu_ticks; o->start_tick = p->start_tick; o->times_scheduled = p->times_scheduled;
    memcpy(o->name, p->name, sizeof o->name);
}

static int32_t h_process_info(uint32_t pid, uint32_t out, uint32_t c, uint32_t d) {
    (void)c; (void)d;
    if (!ok_write(out, sizeof(sys_procinfo_t)) || out == 0) return SYS_EFAULT;
    const PCB *p = process_get((int)pid);
    if (!p) return SYS_ESRCH;
    sys_procinfo_t info; fill_info(p, &info);
    return copy_out(out, &info, sizeof info);
}

static int32_t h_process_list(uint32_t out, uint32_t max, uint32_t c, uint32_t d) {
    (void)c; (void)d;
    int pids[PROC_MAX]; uint32_t n = 0;
    for (size_t i = 0; i < PROC_MAX; i++) {
        const PCB *p = process_slot(i);
        if (!p) continue;
        uint32_t j = n++;
        while (j > 0 && pids[j - 1] > p->pid) { pids[j] = pids[j - 1]; j--; }      /* sorted by pid */
        pids[j] = p->pid;
    }
    if (out == 0) return (int32_t)n;                               /* just count */
    if (max > PROC_MAX) max = PROC_MAX;
    uint32_t k = n < max ? n : max;
    int32_t rc = copy_out(out, pids, k * (uint32_t)sizeof(int));
    return rc ? rc : (int32_t)k;
}

static int32_t h_suspend(uint32_t pid, uint32_t b, uint32_t c, uint32_t d) { (void)b; (void)c; (void)d; return proc_err(process_suspend((int)pid)); }
static int32_t h_resume(uint32_t pid, uint32_t b, uint32_t c, uint32_t d)  { (void)b; (void)c; (void)d; return proc_err(process_resume((int)pid)); }
static int32_t h_setprio(uint32_t pid, uint32_t prio, uint32_t c, uint32_t d) { (void)c; (void)d; return proc_err(process_set_priority((int)pid, (int)prio)); }

/* ---------- files ---------- */
static fd_entry_t *fd_lookup(int fd) {
    proc_res_t *r = res_get(false);
    if (!r || fd < 3 || fd >= SYS_MAX_FDS || !r->fds[fd - 3].used) return NULL;
    return &r->fds[fd - 3];
}

/* The descriptor remembers the path AND the inode: if the name now refers to something else, it is stale. */
static int32_t fd_check(const fd_entry_t *e, fs_stat_t *st) {
    int rc = fs_stat(e->path, st);
    if (rc == FS_ERR_NOT_FOUND || rc == FS_ERR_NOT_DIR) return SYS_ESTALE;
    if (rc != FS_OK) return rc;
    if (st->ino != e->ino) return SYS_ESTALE;
    if (st->type == FS_DIR) return SYS_EISDIR;
    return 0;
}

static int32_t h_open(uint32_t path_ptr, uint32_t flags, uint32_t c, uint32_t d) {
    (void)c; (void)d;
    char path[FS_PATH_MAX], canon[FS_PATH_MAX];
    int32_t rc = copy_str(path, sizeof path, path_ptr);
    if (rc) return rc;
    if (!(flags & (SYS_O_READ | SYS_O_WRITE)) || (flags & ~(uint32_t)63)) return SYS_EINVAL;
    if ((flags & (SYS_O_TRUNC | SYS_O_APPEND)) && !(flags & SYS_O_WRITE)) return SYS_EINVAL;
    proc_res_t *r = res_get(true);
    if (!r) return SYS_ENOMEM;
    int slot = -1;
    for (int i = 0; i < SYS_MAX_FDS - 3; i++) if (!r->fds[i].used) { slot = i; break; }
    if (slot < 0) return SYS_EMFILE;

    rc = fs_resolve(path, canon, sizeof canon);
    if (rc != FS_OK) return rc;
    fs_stat_t st;
    rc = fs_stat(canon, &st);
    if (rc == FS_ERR_NOT_FOUND && (flags & SYS_O_CREATE)) {
        rc = fs_create(canon);
        if (rc != FS_OK) return rc;
        rc = fs_stat(canon, &st);
    } else if (rc == FS_OK && (flags & SYS_O_CREATE) && (flags & SYS_O_EXCL)) return SYS_EEXIST;
    if (rc != FS_OK) return rc;
    if (st.type == FS_DIR) return SYS_EISDIR;
    if (flags & SYS_O_READ) { rc = fs_read(canon, NULL, 0, 0, NULL); if (rc != FS_OK) return rc; }       /* permission probe */
    if (flags & SYS_O_WRITE) { rc = fs_can_write(canon); if (rc != FS_OK) return rc; }
    if (flags & SYS_O_TRUNC) { rc = fs_truncate(canon, 0); if (rc != FS_OK) return rc; }

    fd_entry_t *e = &r->fds[slot];
    memset(e, 0, sizeof *e);
    e->used = true; e->flags = flags; e->ino = st.ino; e->offset = 0;
    memcpy(e->path, canon, strlen(canon) + 1);
    return slot + 3;
}

static int32_t h_close(uint32_t fd, uint32_t b, uint32_t c, uint32_t d) {
    (void)b; (void)c; (void)d;
    fd_entry_t *e = fd_lookup((int)fd);
    if (!e) return SYS_EBADF;
    e->used = false;
    return 0;
}

static int32_t console_write(uint32_t buf, uint32_t n) {
    if (!ok_read(buf, n)) return SYS_EFAULT;
    const char *p = (const char *)(uintptr_t)buf;
    for (uint32_t i = 0; i < n; i++) screen_putc(p[i]);
    return (int32_t)n;
}

static int32_t h_write(uint32_t fd, uint32_t buf, uint32_t n, uint32_t d) {
    (void)d;
    if (n > 0x7FFFFFFFu) return SYS_EINVAL;
    if (fd == 1 || fd == 2) return console_write(buf, n);          /* stdout/stderr = the screen (redirectable) */
    fd_entry_t *e = fd_lookup((int)fd);
    if (!e || !(e->flags & SYS_O_WRITE)) return SYS_EBADF;
    if (!ok_read(buf, n)) return SYS_EFAULT;
    fs_stat_t st; int32_t rc = fd_check(e, &st);
    if (rc) return rc;
    if (e->flags & SYS_O_APPEND) e->offset = (uint32_t)st.size;
    if (n == 0) return 0;
    rc = fs_write_at(e->path, e->offset, (const void *)(uintptr_t)buf, n);
    if (rc != FS_OK) return rc;
    e->offset += n;
    return (int32_t)n;
}

static int32_t h_read(uint32_t fd, uint32_t buf, uint32_t n, uint32_t d) {
    (void)d;
    fd_entry_t *e = NULL;
    if (fd != 0) {                                                  /* the descriptor is checked before the pointer */
        e = fd_lookup((int)fd);
        if (!e || !(e->flags & SYS_O_READ)) return SYS_EBADF;
    }
    if (n > 0x7FFFFFFFu) return SYS_EINVAL;
    if (!ok_write(buf, n)) return SYS_EFAULT;
    if (!e) {                                                       /* stdin: the keyboard, no echo; blocks for the first key */
        if (n == 0) return 0;
        char *out = (char *)(uintptr_t)buf; uint32_t got = 0;
        uint32_t f = irq_save();
        while (got == 0 || (got < n && keyboard_has_key())) {
            int k = keyboard_getkey();                              /* sti;hlt inside until a key arrives */
            if (k < 0x100) out[got++] = (char)k;                    /* arrows etc. are dropped */
        }
        irq_set(f);                                                 /* keyboard_getkey leaves IF on; restore ours */
        return (int32_t)got;
    }
    fs_stat_t st; int32_t rc = fd_check(e, &st);
    if (rc) return rc;
    size_t got = 0;
    rc = fs_read(e->path, (void *)(uintptr_t)buf, n, e->offset, &got);
    if (rc != FS_OK) return rc;
    e->offset += (uint32_t)got;
    return (int32_t)got;
}

static int32_t h_lseek(uint32_t fd, uint32_t off, uint32_t whence, uint32_t d) {
    (void)d;
    fd_entry_t *e = fd_lookup((int)fd);
    if (!e) return SYS_EBADF;
    fs_stat_t st; int32_t rc = fd_check(e, &st);
    if (rc) return rc;
    int64_t base = whence == SYS_SEEK_SET ? 0 : whence == SYS_SEEK_CUR ? (int64_t)e->offset : whence == SYS_SEEK_END ? (int64_t)st.size : -1;
    if (base < 0) return SYS_EINVAL;
    int64_t pos = base + (int32_t)off;
    if (pos < 0 || pos > (int64_t)FS_MAX_FILE_SIZE) return SYS_EINVAL;
    e->offset = (uint32_t)pos;
    return (int32_t)pos;
}

static int32_t h_fdinfo(uint32_t fd, uint32_t out, uint32_t c, uint32_t d) {
    (void)c; (void)d;
    if (out == 0 || !ok_write(out, sizeof(sys_fdinfo_t))) return SYS_EFAULT;
    sys_fdinfo_t info; memset(&info, 0, sizeof info);
    info.fd = (int32_t)fd;
    if (fd <= 2) {
        info.flags = fd == 0 ? SYS_O_READ : SYS_O_WRITE;
        memcpy(info.path, "console", 8);
    } else {
        fd_entry_t *e = fd_lookup((int)fd);
        if (!e) return SYS_EBADF;
        fs_stat_t st; int32_t rc = fd_check(e, &st);
        info.flags = e->flags; info.offset = e->offset; info.stale = (rc == SYS_ESTALE);
        info.size = rc == 0 ? (uint32_t)st.size : 0;
        memcpy(info.path, e->path, strlen(e->path) + 1);
    }
    return copy_out(out, &info, sizeof info);
}

static int32_t h_fstat(uint32_t fd, uint32_t out, uint32_t c, uint32_t d) {
    (void)c; (void)d;
    fd_entry_t *e = fd_lookup((int)fd);
    if (!e) return SYS_EBADF;
    fs_stat_t st; int32_t rc = fd_check(e, &st);
    if (rc) return rc;
    return copy_out(out, &st, sizeof st);
}

/* Path-based calls share one pattern: copy the path in, call MiniFS. */
#define WITH_PATH(var, ptr) char var[FS_PATH_MAX]; { int32_t _rc = copy_str(var, sizeof var, ptr); if (_rc) return _rc; }

static int32_t h_create(uint32_t p, uint32_t b, uint32_t c, uint32_t d) { (void)b; (void)c; (void)d; WITH_PATH(path, p); return fs_create(path); }
static int32_t h_delete(uint32_t p, uint32_t b, uint32_t c, uint32_t d) { (void)b; (void)c; (void)d; WITH_PATH(path, p); return fs_remove(path); }
static int32_t h_rmdir(uint32_t p, uint32_t b, uint32_t c, uint32_t d)  { (void)b; (void)c; (void)d; WITH_PATH(path, p); return fs_rmdir(path); }
static int32_t h_touch(uint32_t p, uint32_t b, uint32_t c, uint32_t d)  { (void)b; (void)c; (void)d; WITH_PATH(path, p); return fs_touch(path); }
static int32_t h_chdir(uint32_t p, uint32_t b, uint32_t c, uint32_t d)  { (void)b; (void)c; (void)d; WITH_PATH(path, p); return fs_chdir(path); }
static int32_t h_mkdir(uint32_t p, uint32_t flags, uint32_t c, uint32_t d) {
    (void)c; (void)d; WITH_PATH(path, p);
    return (flags & SYS_MKDIR_PARENTS) ? fs_mkdir_parents(path) : fs_mkdir(path);
}
static int32_t h_chmod(uint32_t p, uint32_t mode, uint32_t c, uint32_t d) { (void)c; (void)d; WITH_PATH(path, p); if (mode > 0xFFFF) return SYS_EINVAL; return fs_chmod(path, (uint16_t)mode); }
static int32_t h_truncate(uint32_t p, uint32_t size, uint32_t c, uint32_t d) { (void)c; (void)d; WITH_PATH(path, p); return fs_truncate(path, size); }
static int32_t h_rename(uint32_t from, uint32_t to, uint32_t c, uint32_t d) {
    (void)c; (void)d; WITH_PATH(a, from); WITH_PATH(b, to);
    return fs_rename(a, b);
}
static int32_t h_stat(uint32_t p, uint32_t out, uint32_t c, uint32_t d) {
    (void)c; (void)d; WITH_PATH(path, p);
    if (out == 0 || !ok_write(out, sizeof(fs_stat_t))) return SYS_EFAULT;
    fs_stat_t st; int rc = fs_stat(path, &st);
    return rc ? rc : copy_out(out, &st, sizeof st);
}

typedef struct { fs_dirent_t *out; uint32_t max, n; } readdir_ctx_t;
static bool readdir_cb(const fs_dirent_t *e, void *vc) {
    readdir_ctx_t *c = vc;
    if (c->out && c->n < c->max) c->out[c->n] = *e;
    c->n++;
    return true;
}
static int32_t h_readdir(uint32_t p, uint32_t out, uint32_t max, uint32_t d) {
    (void)d; WITH_PATH(path, p);
    if (max > FS_MAX_NODES) max = FS_MAX_NODES;
    if (out == 0) max = 0;                                         /* count only */
    else if (!ok_write(out, max * (uint32_t)sizeof(fs_dirent_t))) return SYS_EFAULT;
    readdir_ctx_t c = { (fs_dirent_t *)(uintptr_t)out, max, 0 };
    int rc = fs_readdir(path, readdir_cb, &c);
    if (rc != FS_OK) return rc;
    return (int32_t)(out == 0 ? c.n : (c.n < max ? c.n : max));
}

static int32_t h_getcwd(uint32_t out, uint32_t size, uint32_t c, uint32_t d) {
    (void)c; (void)d;
    char cwd[FS_PATH_MAX]; fs_getcwd(cwd, sizeof cwd);
    uint32_t len = (uint32_t)strlen(cwd);
    if (size < len + 1) return SYS_EINVAL;                         /* buffer too small */
    int32_t rc = copy_out(out, cwd, len + 1);
    return rc ? rc : (int32_t)len;
}

/* ---------- memory ---------- */
static int32_t h_memory_alloc(uint32_t size, uint32_t b, uint32_t c, uint32_t d) {
    (void)b; (void)c; (void)d;
    if (size == 0) return SYS_EINVAL;
    proc_res_t *r = res_get(true);
    if (!r) return SYS_ENOMEM;
    int slot = -1;
    for (int i = 0; i < SYS_MAX_ALLOCS; i++) if (!r->allocs[i].ptr) { slot = i; break; }
    if (slot < 0) return SYS_ELIMIT;
    void *p = kmalloc(size);
    if (!p) return SYS_ENOMEM;
    r->allocs[slot].ptr = p; r->allocs[slot].size = size;
    return (int32_t)(uintptr_t)p;                                  /* RAM is capped at 1 GiB, so this is never negative */
}

static int32_t h_memory_free(uint32_t ptr, uint32_t b, uint32_t c, uint32_t d) {
    (void)b; (void)c; (void)d;
    proc_res_t *r = res_get(false);
    if (!r || ptr == 0) return SYS_EINVAL;
    for (int i = 0; i < SYS_MAX_ALLOCS; i++) {
        if (r->allocs[i].ptr && (uintptr_t)r->allocs[i].ptr == ptr) {
            int rc = kfree(r->allocs[i].ptr);
            r->allocs[i].ptr = NULL; r->allocs[i].size = 0;
            return rc == HEAP_OK ? 0 : SYS_EINVAL;
        }
    }
    return SYS_EINVAL;                                             /* not an allocation of THIS process (or already freed) */
}

/* ---------- information ---------- */
static int32_t h_sysinfo(uint32_t out, uint32_t b, uint32_t c, uint32_t d) {
    (void)b; (void)c; (void)d;
    if (out == 0 || !ok_write(out, sizeof(sys_info_t))) return SYS_EFAULT;
    sys_info_t i; memset(&i, 0, sizeof i);
    memory_summary_t m; memory_summary(&m);
    heap_stats_t h; heap_stats(&h);
    sched_stats_t s; sched_stats(&s);
    fs_usage_t u; fs_usage(&u);
    i.uptime_ticks = timer_ticks(); i.ticks_per_sec = TIMER_HZ;
    i.ram_bytes = m.ram_bytes; i.ram_free_bytes = m.free_bytes;
    i.heap_total = (uint32_t)h.total_bytes; i.heap_used = (uint32_t)(h.payload_used + h.overhead_bytes); i.heap_free = (uint32_t)h.payload_free;
    for (size_t k = 0; k < PROC_MAX; k++) if (process_slot(k) && process_slot(k)->state != PROC_TERMINATED) i.processes++;
    i.context_switches = s.context_switches;
    i.fs_nodes = u.nodes; i.fs_max_nodes = u.max_nodes; i.fs_data_bytes = (uint32_t)u.data_bytes;
    i.syscalls_total = stats.total; i.syscalls_failed = stats.failed;
    return copy_out(out, &i, sizeof i);
}

static int32_t h_time(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { (void)a; (void)b; (void)c; (void)d; return (int32_t)timer_ticks(); }

static int32_t h_stats(uint32_t out, uint32_t b, uint32_t c, uint32_t d) {
    (void)b; (void)c; (void)d;
    if (out == 0) return SYS_EFAULT;
    return copy_out(out, &stats, sizeof stats);
}


/* ---------- identity, accounts and ownership (Phase 10) ----------
 * Passwords are copied into kernel buffers, used once and wiped. Who may do what is decided by the
 * user manager (kernel/security/users.c) from the effective uid, never by the caller's claims. */
static void wipe_buf(char *b, size_t n) { volatile char *v = b; while (n--) *v++ = 0; }

static int32_t copy_user_name(char *dst, uint32_t ptr) {
    int32_t rc = copy_str(dst, USER_NAME_MAX + 1, ptr);
    return rc == SYS_EPATHTOOLONG ? SYS_ENAMETOOLONG : rc;
}
static int32_t copy_password(char *dst, uint32_t ptr) {               /* dst holds USER_PASS_MAX + 2 */
    int32_t rc = copy_str(dst, USER_PASS_MAX + 2, ptr);
    return rc == SYS_EPATHTOOLONG ? SYS_EINVAL : rc;
}

static int32_t h_getuid(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { (void)a; (void)b; (void)c; (void)d; return users_getuid(); }
static int32_t h_getcred(uint32_t out, uint32_t b, uint32_t c, uint32_t d) {
    (void)b; (void)c; (void)d;
    if (out == 0 || !ok_write(out, sizeof(cred_info_t))) return SYS_EFAULT;
    cred_info_t ci; users_cred(&ci);
    return copy_out(out, &ci, sizeof ci);
}
static int32_t h_umask(uint32_t m, uint32_t b, uint32_t c, uint32_t d) { (void)b; (void)c; (void)d; return users_umask((int32_t)m); }

static int32_t h_chown(uint32_t p, uint32_t uid, uint32_t gid, uint32_t d) {
    (void)d; WITH_PATH(path, p);
    if (users_euid() != (int)UID_ROOT) return SYS_EACCES;              /* only root gives files away */
    if (uid != UID_KEEP && !users_uid_exists(uid)) return SYS_ENOUSER;
    if (gid != UID_KEEP && !users_gid_exists(gid)) return SYS_ENOUSER;
    fs_stat_t st; int rc = fs_stat(path, &st);
    if (rc != FS_OK) return rc;
    return fs_chown(path, uid == UID_KEEP ? st.uid : (uint16_t)uid, gid == UID_KEEP ? st.gid : (uint16_t)gid);
}

/* name + password calls: login, su, verify */
typedef int (*name_pw_fn)(const char *, const char *);
static int32_t name_pw_call(uint32_t name_ptr, uint32_t pw_ptr, name_pw_fn fn) {
    char name[USER_NAME_MAX + 1], pw[USER_PASS_MAX + 2];
    int32_t rc = copy_user_name(name, name_ptr);
    if (rc) return rc;
    rc = copy_password(pw, pw_ptr);
    if (rc == 0) rc = fn(name, pw);
    wipe_buf(pw, sizeof pw);
    return rc;
}
static int32_t h_login(uint32_t n, uint32_t pw, uint32_t c, uint32_t d)  { (void)c; (void)d; return name_pw_call(n, pw, users_login); }
static int32_t h_su(uint32_t n, uint32_t pw, uint32_t c, uint32_t d)     { (void)c; (void)d; return name_pw_call(n, pw, users_su); }
static int32_t h_verify(uint32_t n, uint32_t pw, uint32_t c, uint32_t d) { (void)c; (void)d; return name_pw_call(n, pw, users_verify); }
static int32_t h_logout(uint32_t a, uint32_t b, uint32_t c, uint32_t d)  { (void)a; (void)b; (void)c; (void)d; return users_logout(); }
static int32_t h_sudo_end(uint32_t a, uint32_t b, uint32_t c, uint32_t d){ (void)a; (void)b; (void)c; (void)d; return users_sudo_end(); }
static int32_t h_sudo_begin(uint32_t pw_ptr, uint32_t b, uint32_t c, uint32_t d) {
    (void)b; (void)c; (void)d;
    char pw[USER_PASS_MAX + 2];
    int32_t rc = copy_password(pw, pw_ptr);
    if (rc == 0) rc = users_sudo_begin(pw);
    wipe_buf(pw, sizeof pw);
    return rc;
}

static int32_t h_user_get(uint32_t index, uint32_t out, uint32_t c, uint32_t d) {
    (void)c; (void)d;
    if (out == 0 || !ok_write(out, sizeof(user_info_t))) return SYS_EFAULT;
    user_info_t u; int rc = users_get(index, &u);
    return rc ? rc : copy_out(out, &u, sizeof u);
}
static int32_t h_user_lookup(uint32_t name_ptr, uint32_t uid, uint32_t out, uint32_t d) {
    (void)d;
    if (out == 0 || !ok_write(out, sizeof(user_info_t))) return SYS_EFAULT;
    char name[USER_NAME_MAX + 1];
    if (name_ptr) { int32_t rc = copy_user_name(name, name_ptr); if (rc) return rc; }
    user_info_t u; int rc = users_lookup(name_ptr ? name : NULL, (int32_t)uid, &u);
    return rc ? rc : copy_out(out, &u, sizeof u);
}
static int32_t h_user_add(uint32_t name_ptr, uint32_t pw_ptr, uint32_t flags, uint32_t d) {
    (void)d;
    char name[USER_NAME_MAX + 1], pw[USER_PASS_MAX + 2]; uint16_t uid = 0;
    int32_t rc = copy_user_name(name, name_ptr);
    if (rc) return rc;
    rc = copy_password(pw, pw_ptr);
    if (rc == 0) rc = users_add(name, pw, flags, &uid);
    wipe_buf(pw, sizeof pw);
    return rc ? rc : (int32_t)uid;
}
static int32_t h_user_del(uint32_t name_ptr, uint32_t rm_home, uint32_t c, uint32_t d) {
    (void)c; (void)d;
    char name[USER_NAME_MAX + 1];
    int32_t rc = copy_user_name(name, name_ptr);
    return rc ? rc : users_del(name, rm_home != 0);
}
static int32_t h_user_passwd(uint32_t name_ptr, uint32_t old_ptr, uint32_t new_ptr, uint32_t d) {
    (void)d;
    char name[USER_NAME_MAX + 1], oldpw[USER_PASS_MAX + 2], newpw[USER_PASS_MAX + 2];
    int32_t rc = copy_user_name(name, name_ptr);
    if (rc) return rc;
    if (old_ptr && (rc = copy_password(oldpw, old_ptr)) != 0) return rc;
    rc = copy_password(newpw, new_ptr);
    if (rc == 0) rc = users_set_password(name, old_ptr ? oldpw : NULL, newpw);
    if (old_ptr) wipe_buf(oldpw, sizeof oldpw);
    wipe_buf(newpw, sizeof newpw);
    return rc;
}
static int32_t h_user_mod(uint32_t name_ptr, uint32_t op, uint32_t val_ptr, uint32_t d) {
    (void)d;
    char name[USER_NAME_MAX + 1], val[USER_COMMENT_MAX + 2];
    int32_t rc = copy_user_name(name, name_ptr);
    if (rc) return rc;
    if (val_ptr) { rc = copy_str(val, sizeof val, val_ptr); if (rc == SYS_EPATHTOOLONG) return SYS_EINVAL; if (rc) return rc; }
    return users_modify(name, (int)op, val_ptr ? val : NULL);
}
static int32_t h_audit_get(uint32_t index, uint32_t out, uint32_t c, uint32_t d) {
    (void)c; (void)d;
    if (out == 0) return users_audit_get(index, NULL);
    if (!ok_write(out, sizeof(audit_event_t))) return SYS_EFAULT;
    audit_event_t e; int rc = users_audit_get(index, &e);
    return rc ? rc : copy_out(out, &e, sizeof e);
}


/* ---------- kernel information, power, protection, RAM-disk (Phase 11) ---------- */
static bool is_root(void) { return users_euid() == (int)UID_ROOT; }

static int32_t h_kinfo(uint32_t what, uint32_t index, uint32_t out, uint32_t size) {
    uint32_t need = kinfo_size((int)what);
    if (need == 0 || need > KI_MAX_STRUCT || size < need) return SYS_EINVAL;
    if (out == 0 || !ok_write(out, need)) return SYS_EFAULT;
    uint64_t tmp[KI_MAX_STRUCT / 8 + 1];
    memset(tmp, 0, sizeof tmp);
    if (what == KI_CPUID) { if (!ok_read(out, need)) return SYS_EFAULT; memcpy(tmp, (const void *)(uintptr_t)out, need); }   /* leaf/subleaf are input */
    int32_t rc = kinfo_get((int)what, index, tmp, need);
    return rc ? rc : copy_out(out, tmp, need);
}
static int32_t h_power(uint32_t op, uint32_t b, uint32_t c, uint32_t d) {
    (void)c; (void)d;
    if (op == POWER_BEEP_ON) return kinfo_beep(true, b);            /* the speaker is harmless: no privilege needed */
    if (op == POWER_BEEP_OFF) return kinfo_beep(false, 0);
    if (!is_root()) return SYS_EACCES;                              /* only root (or sudo) may stop the machine */
    return kinfo_power((int)op);
}
/* ---------- kernel log (Phase 12) ---------- */
static int32_t h_log_get(uint32_t which, uint32_t index, uint32_t out, uint32_t d) {
    (void)d;
    if (which != SYS_LOG_RING && which != SYS_LOG_BOOT) return SYS_EINVAL;
    bool root = is_root();
    if (out == 0) return which == SYS_LOG_BOOT ? klog_boot_get(index, NULL) : klog_get(index, root, NULL);
    if (!ok_write(out, sizeof(log_entry_t))) return SYS_EFAULT;
    log_entry_t e;
    int rc = which == SYS_LOG_BOOT ? klog_boot_get(index, &e) : klog_get(index, root, &e);
    if (rc != 0) return SYS_ENOENT;
    if (which == SYS_LOG_BOOT && !root && klog_private_source(e.src)) return SYS_ENOENT;     /* the boot copy obeys the same privacy rule */
    return copy_out(out, &e, sizeof e);
}
static int32_t h_log_ctl(uint32_t op, uint32_t arg, uint32_t out, uint32_t d) {
    (void)d;
    int uid = users_euid();
    switch (op) {
        case SYS_LOGCTL_STATS: {
            if (out == 0 || !ok_write(out, sizeof(log_stats_t))) return SYS_EFAULT;
            log_stats_t s; klog_stats(is_root(), &s);
            return copy_out(out, &s, sizeof s);
        }
        case SYS_LOGCTL_SET_LEVEL:
            if (!is_root()) return SYS_EACCES;
            if (arg >= LOG_LEVELS) return SYS_EINVAL;
            klogf(LOG_INFO, "log", "minimum level set to %s by uid %d", klog_level_name((int)arg), uid);   /* logged BEFORE the change so a raise is visible */
            klog_set_level((int)arg);
            return 0;
        case SYS_LOGCTL_CLEAR:
            if (!is_root()) return SYS_EACCES;
            klog_clear();
            klogf(LOG_WARN, "log", "log cleared by uid %d", uid);
            return 0;
        case SYS_LOGCTL_TRACE:
            if (!is_root()) return SYS_EACCES;
            if (arg > 1) return SYS_EINVAL;
            klog_set_trace(arg != 0);
            klogf(LOG_INFO, "log", "syscall trace %s by uid %d", arg ? "ON" : "OFF", uid);
            return 0;
        case SYS_LOGCTL_SCHED:
            if (!is_root()) return SYS_EACCES;
            if (arg > 1) return SYS_EINVAL;
            klog_set_sched_trace(arg != 0);
            klogf(LOG_INFO, "log", "scheduler trace %s by uid %d", arg ? "ON" : "OFF", uid);
            return 0;
        default: return SYS_EINVAL;
    }
}
static int32_t h_lock(uint32_t p, uint32_t on, uint32_t c, uint32_t d) { (void)c; (void)d; WITH_PATH(path, p); return fs_set_lock(path, on != 0); }
static int32_t h_protect(uint32_t p, uint32_t on, uint32_t c, uint32_t d) { (void)c; (void)d; WITH_PATH(path, p); return fs_set_protect(path, on != 0); }
static int32_t h_blk_info(uint32_t out, uint32_t b, uint32_t c, uint32_t d) {
    (void)b; (void)c; (void)d;
    if (out == 0 || !ok_write(out, sizeof(ramdisk_info_t))) return SYS_EFAULT;
    ramdisk_info_t i; ramdisk_info(&i);
    return copy_out(out, &i, sizeof i);
}
static int32_t h_blk_read(uint32_t sector, uint32_t buf, uint32_t c, uint32_t d) {
    (void)c; (void)d;
    if (!is_root()) return SYS_EACCES;                              /* raw blocks bypass file permissions */
    if (buf == 0 || !ok_write(buf, RAMDISK_SECTOR)) return SYS_EFAULT;
    uint8_t tmp[RAMDISK_SECTOR];
    int32_t rc = ramdisk_read(sector, tmp);
    return rc ? rc : copy_out(buf, tmp, sizeof tmp);
}
static int32_t h_blk_write(uint32_t sector, uint32_t buf, uint32_t c, uint32_t d) {
    (void)c; (void)d;
    if (!is_root()) return SYS_EACCES;
    if (buf == 0 || !ok_read(buf, RAMDISK_SECTOR)) return SYS_EFAULT;
    return ramdisk_write(sector, (const void *)(uintptr_t)buf);
}
static int32_t h_fs_save(uint32_t bytes_out, uint32_t b, uint32_t c, uint32_t d) {
    (void)b; (void)c; (void)d;
    if (!is_root()) return SYS_EACCES;
    if (bytes_out && !ok_write(bytes_out, 4)) return SYS_EFAULT;
    uint32_t nodes = 0, bytes = 0;
    int32_t rc = fsimage_save(&nodes, &bytes);
    if (rc) { klogf(LOG_ERROR, "disk", "savefs failed: %s", sys_strerror(rc)); return rc; }
    klogf(LOG_INFO, "disk", "file system saved to the RAM-disk: %u nodes, %u bytes", nodes, bytes);
    if (bytes_out) copy_out(bytes_out, &bytes, 4);
    return (int32_t)nodes;
}
static int32_t h_fs_load(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    (void)a; (void)b; (void)c; (void)d;
    if (!is_root()) return SYS_EACCES;
    uint32_t nodes = 0;
    int32_t rc = fsimage_load(&nodes);
    if (rc) { klogf(LOG_ERROR, "disk", "loadfs failed: %s", sys_strerror(rc)); return rc; }
    klogf(LOG_WARN, "disk", "file system REPLACED from the RAM-disk image: %u nodes (by uid %d)", nodes, users_euid());
    users_fs_sync();                                                /* accounts may have changed since the image was saved */
    return (int32_t)nodes;
}

/* ---------- dispatch ---------- */
typedef int32_t (*sys_handler_t)(uint32_t, uint32_t, uint32_t, uint32_t);
static const struct { const char *name; sys_handler_t fn; } table[SYS_COUNT] = {
    [SYS_EXIT] = { "exit", h_exit }, [SYS_GETPID] = { "getpid", h_getpid }, [SYS_GETPPID] = { "getppid", h_getppid },
    [SYS_YIELD] = { "yield", h_yield }, [SYS_SLEEP] = { "sleep", h_sleep },
    [SYS_PROCESS_CREATE] = { "process_create", h_process_create }, [SYS_PROCESS_KILL] = { "process_kill", h_process_kill },
    [SYS_PROCESS_WAIT] = { "process_wait", h_process_wait }, [SYS_PROCESS_INFO] = { "process_info", h_process_info },
    [SYS_PROCESS_LIST] = { "process_list", h_process_list }, [SYS_PROCESS_SUSPEND] = { "process_suspend", h_suspend },
    [SYS_PROCESS_RESUME] = { "process_resume", h_resume }, [SYS_SETPRIORITY] = { "setpriority", h_setprio },
    [SYS_WRITE] = { "write", h_write }, [SYS_READ] = { "read", h_read }, [SYS_OPEN] = { "open", h_open },
    [SYS_CLOSE] = { "close", h_close }, [SYS_LSEEK] = { "lseek", h_lseek },
    [SYS_CREATE] = { "create", h_create }, [SYS_DELETE] = { "delete", h_delete }, [SYS_MKDIR] = { "mkdir", h_mkdir },
    [SYS_RMDIR] = { "rmdir", h_rmdir }, [SYS_RENAME] = { "rename", h_rename }, [SYS_STAT] = { "stat", h_stat },
    [SYS_FSTAT] = { "fstat", h_fstat }, [SYS_READDIR] = { "readdir", h_readdir },
    [SYS_GETCWD] = { "getcwd", h_getcwd }, [SYS_CHDIR] = { "chdir", h_chdir }, [SYS_CHMOD] = { "chmod", h_chmod },
    [SYS_TOUCH] = { "touch", h_touch }, [SYS_TRUNCATE] = { "truncate", h_truncate },
    [SYS_MEMORY_ALLOC] = { "memory_alloc", h_memory_alloc }, [SYS_MEMORY_FREE] = { "memory_free", h_memory_free },
    [SYS_SYSINFO] = { "sysinfo", h_sysinfo }, [SYS_TIME] = { "time", h_time }, [SYS_FDINFO] = { "fdinfo", h_fdinfo },
    [SYS_STATS] = { "stats", h_stats },
    [SYS_GETUID] = { "getuid", h_getuid }, [SYS_GETCRED] = { "getcred", h_getcred }, [SYS_CHOWN] = { "chown", h_chown },
    [SYS_UMASK] = { "umask", h_umask }, [SYS_LOGIN] = { "login", h_login }, [SYS_LOGOUT] = { "logout", h_logout },
    [SYS_SU] = { "su", h_su }, [SYS_SUDO_BEGIN] = { "sudo_begin", h_sudo_begin }, [SYS_SUDO_END] = { "sudo_end", h_sudo_end },
    [SYS_VERIFY] = { "verify", h_verify }, [SYS_USER_GET] = { "user_get", h_user_get }, [SYS_USER_LOOKUP] = { "user_lookup", h_user_lookup },
    [SYS_USER_ADD] = { "user_add", h_user_add }, [SYS_USER_DEL] = { "user_del", h_user_del }, [SYS_USER_PASSWD] = { "user_passwd", h_user_passwd },
    [SYS_USER_MOD] = { "user_mod", h_user_mod }, [SYS_AUDIT_GET] = { "audit_get", h_audit_get },
    [SYS_KINFO] = { "kinfo", h_kinfo }, [SYS_POWER] = { "power", h_power }, [SYS_PROTECT] = { "protect", h_protect },
    [SYS_BLK_INFO] = { "blk_info", h_blk_info }, [SYS_BLK_READ] = { "blk_read", h_blk_read }, [SYS_BLK_WRITE] = { "blk_write", h_blk_write },
    [SYS_FS_SAVE] = { "fs_save", h_fs_save }, [SYS_FS_LOAD] = { "fs_load", h_fs_load },
    [SYS_LOG_GET] = { "log_get", h_log_get }, [SYS_LOG_CTL] = { "log_ctl", h_log_ctl }, [SYS_LOCK] = { "lock", h_lock },
};

const char *sys_name(unsigned n) { return n < SYS_COUNT && table[n].name ? table[n].name : "?"; }

/* One trace line: name(a1, a2, ...) = result. Small numbers print in decimal, big ones (addresses) in hex. */
static void trace_call(uint32_t n, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, int32_t rc) {
    uint32_t a[4] = { a1, a2, a3, a4 }; char args[40]; size_t w = 0;
    int last = -1;
    for (int i = 0; i < 4; i++) if (a[i]) last = i;                       /* trailing zero arguments are not shown */
    for (int i = 0; i <= last; i++) {
        char piece[14]; size_t k = 0;
        if (a[i] < 65536) { char t[8]; size_t m = 0; uint32_t v = a[i]; do { t[m++] = (char)('0' + v % 10); v /= 10; } while (v); while (m) piece[k++] = t[--m]; }
        else { const char *h = "0123456789abcdef"; piece[k++] = '0'; piece[k++] = 'x'; bool started = false; for (int sh = 28; sh >= 0; sh -= 4) { uint32_t nib = (a[i] >> sh) & 15; if (nib || started || sh == 0) { piece[k++] = h[nib]; started = true; } } }
        if (w + k + 2 >= sizeof args) break;
        if (i) { args[w++] = ','; args[w++] = ' '; }
        for (size_t q = 0; q < k; q++) args[w++] = piece[q];
    }
    args[w] = '\0';
    if (rc < 0) klog_tracef(false, process_current_pid(), "%s(%s) = %d %s", sys_name(n), args, rc, sys_errname(rc));
    else klog_tracef(false, process_current_pid(), "%s(%s) = %d", sys_name(n), args, rc);
}

int32_t syscall_dispatch(uint32_t n, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) {
    stats.total++;
    if (n >= SYS_COUNT || !table[n].fn) { stats.failed++; return SYS_ENOSYS; }
    stats.calls[n]++;
    int32_t rc = table[n].fn(a1, a2, a3, a4);
    if (rc < 0) { stats.errors[n]++; stats.failed++; }
    if (klog_syscall_tracing() && n != SYS_LOG_GET && n != SYS_LOG_CTL)      /* the log calls are not traced: reading the trace would add to it */
        trace_call(n, a1, a2, a3, a4, rc);
    return rc;
}

/* int 0x80: arguments arrive in the saved registers; the result goes back into the saved eax. */
void syscall_interrupt(registers_t *r) {
    r->eax = (uint32_t)syscall_dispatch(r->eax, r->ebx, r->ecx, r->edx, r->esi);
}

void syscall_init(void) {
    memset(&stats, 0, sizeof stats);
    interrupt_register(SYSCALL_VECTOR, syscall_interrupt);
    process_set_cleanup_hook(syscall_cleanup);
}
