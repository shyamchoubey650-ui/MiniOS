#ifndef LOGGER_H
#define LOGGER_H
/* Kernel logger (Phase 12): a fixed ring buffer of timestamped, levelled, source-tagged messages.
 *
 * REAL:      every boot message, login/sudo/su/denied event, process start/exit/fault, system-wide action (mkfs, savefs,
 *            loadfs, log changes) is stored here. Syscall tracing and scheduler tracing add entries on request.
 *            Writers are interrupt-safe (the scheduler logs from the timer IRQ); no memory is allocated.
 * LIMITS:    256 entries; the oldest are overwritten (and counted). Messages are cut at 71 characters. Entries are
 *            kept in RAM and lost at reboot unless `logsave` writes them to a file.
 * PRIVACY:   entries from the sources "sec" and "shell" are PRIVATE: only root can read them (klog_get with root = false skips them). */
#include "include/types.h"

#define LOG_DEBUG 0
#define LOG_INFO  1
#define LOG_WARN  2
#define LOG_ERROR 3
#define LOG_LEVELS 4

#define LOG_CAPACITY  256
#define LOG_BOOT_KEEP 96
#define LOG_MSG_MAX   72           /* including the terminating NUL */
#define LOG_SRC_MAX   8

typedef struct {
    uint32_t seq;                  /* 1, 2, 3 ... never reused (gaps show overwritten or filtered entries) */
    uint32_t tick;
    int16_t  pid;                  /* process that wrote it, -1 = kernel/boot/IRQ context */
    uint8_t  level;
    char     src[LOG_SRC_MAX];
    char     msg[LOG_MSG_MAX];
} log_entry_t;

typedef struct {
    uint32_t total_written;        /* entries stored since boot (or since logclear) */
    uint32_t stored;               /* entries currently in the ring */
    uint32_t overwritten;          /* lost because the ring was full */
    uint32_t dropped_below_level;  /* refused because of the minimum level */
    uint32_t per_level[LOG_LEVELS];/* entries currently stored, by level */
    uint32_t capacity, boot_entries;
    uint8_t  min_level;
    bool     trace_syscalls, trace_sched;
    uint32_t trace_count, sched_count;
    uint32_t hidden_private;       /* stored entries the caller is not allowed to read */
} log_stats_t;

void        klog_init(void);                                         /* empty ring, level INFO, tracing off */
void        klog(int level, const char *src, const char *msg);
void        klogf(int level, const char *src, const char *fmt, ...); /* %s %d %u %x %c %% with optional width (%5d, %08x) */
void        klog_pid(int pid, int level, const char *src, const char *msg);   /* when the author is not the current process */
void        klog_set_pid_source(int (*current_pid)(void));
void        klog_mute(bool muted);                                   /* boot self-tests: stored nothing, counted nothing */
void        klog_tracef(bool sched, int pid, const char *fmt, ...);  /* trace entry with formatting (bypasses the minimum level) */

/* Trace entries bypass the minimum level: asking for a trace means you want it. */
void        klog_trace_syscall(int pid, const char *msg);
void        klog_trace_sched(int pid, const char *msg);
bool        klog_syscall_tracing(void);
bool        klog_sched_tracing(void);

int         klog_set_level(int level);                               /* 0 or -1 */
int         klog_level(void);
void        klog_set_trace(bool syscalls);
void        klog_set_sched_trace(bool on);
void        klog_clear(void);

/* Reading. Index 0 = oldest entry the caller may see. Returns the visible count when out == NULL; -1 when index is past the end. */
int         klog_get(uint32_t index, bool root, log_entry_t *out);
int         klog_boot_get(uint32_t index, log_entry_t *out);         /* the snapshot taken by klog_boot_done() */
void        klog_boot_done(void);                                    /* call once, when boot finishes */
void        klog_stats(bool root, log_stats_t *out);

bool        klog_private_source(const char *src);
const char *klog_level_name(int level);                              /* "DEBUG" .. "ERROR" */
int         klog_level_from_name(const char *name);                  /* case-insensitive, also "warning"; -1 if unknown */
size_t      klog_format(const log_entry_t *e, char *out, size_t cap);/* "  12.34 INFO  proc   created pid 4" */
#endif
