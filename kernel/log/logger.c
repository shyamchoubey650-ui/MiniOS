#include "include/logger.h"
#include "include/timer.h"
#include "include/irqflags.h"
#include "include/string.h"

typedef __builtin_va_list va_list;
#define va_start(a, b) __builtin_va_start(a, b)
#define va_arg(a, t)   __builtin_va_arg(a, t)
#define va_end(a)      __builtin_va_end(a)

static log_entry_t ring[LOG_CAPACITY];
static log_entry_t boot_log[LOG_BOOT_KEEP];
static uint32_t next_seq = 1;             /* sequence number of the next entry */
static uint32_t first_seq = 1;            /* sequence number of the oldest entry in the ring */
static uint32_t overwritten, dropped, boot_n, trace_count, sched_count;
static uint8_t  min_level = LOG_INFO;
static bool     trace_on, sched_on, boot_done, muted;
static int    (*pid_source)(void);

static void copy_text(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (src) while (src[i] && i + 1 < cap) { unsigned char c = (unsigned char)src[i]; dst[i] = (c < 32 || c > 126) ? '?' : (char)c; i++; }   /* no control characters in the log */
    dst[i] = '\0';
}
static bool streq_ci(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
        if (!x) return true;
    }
}

void klog_init(void) {
    uint32_t f = irq_save();
    memset(ring, 0, sizeof ring); memset(boot_log, 0, sizeof boot_log);
    next_seq = first_seq = 1; overwritten = dropped = boot_n = trace_count = sched_count = 0;
    min_level = LOG_INFO; trace_on = sched_on = boot_done = muted = false;
    irq_restore(f);
}
void klog_set_pid_source(int (*fn)(void)) { pid_source = fn; }
void klog_mute(bool m) { muted = m; }

static void store(int pid, int level, const char *src, const char *msg, bool is_trace) {
    uint32_t f = irq_save();
    if (muted) { irq_restore(f); return; }
    if (!is_trace && level < (int)min_level) { dropped++; irq_restore(f); return; }
    if (next_seq - first_seq >= LOG_CAPACITY) { first_seq++; overwritten++; }       /* ring full: the oldest entry goes */
    log_entry_t *e = &ring[next_seq % LOG_CAPACITY];
    e->seq = next_seq++; e->tick = timer_ticks(); e->pid = (int16_t)pid;
    e->level = (uint8_t)(level < 0 ? 0 : level >= LOG_LEVELS ? LOG_ERROR : level);
    copy_text(e->src, sizeof e->src, src); copy_text(e->msg, sizeof e->msg, msg);
    if (!boot_done && boot_n < LOG_BOOT_KEEP) boot_log[boot_n++] = *e;               /* boot messages are also kept apart: the ring may wrap later */
    irq_restore(f);
}

void klog_pid(int pid, int level, const char *src, const char *msg) { store(pid, level, src, msg, false); }
void klog(int level, const char *src, const char *msg) { store(pid_source ? pid_source() : -1, level, src, msg, false); }

/* ---------- tiny formatter ---------- */
static size_t put_num(char *o, size_t w, size_t cap, uint32_t v, uint32_t base, int width, char pad, bool neg) {
    char t[34]; size_t n = 0; const char *d = "0123456789abcdef";
    do { t[n++] = d[v % base]; v /= base; } while (v && n < 33);
    if (neg) t[n++] = '-';
    for (int k = (int)n; k < width; k++) if (w + 1 < cap) o[w++] = pad;
    while (n) if (w + 1 < cap) o[w++] = t[--n]; else n--;
    return w;
}
static size_t vformat(char *o, size_t cap, const char *fmt, va_list ap) {
    size_t w = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') { if (w + 1 < cap) o[w++] = *fmt; continue; }
        fmt++;
        char pad = ' '; int width = 0;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; if (width > 30) width = 30; }
        switch (*fmt) {
            case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; size_t n = strlen(s); for (int k = (int)n; k < width; k++) if (w + 1 < cap) o[w++] = ' '; for (size_t k = 0; s[k]; k++) if (w + 1 < cap) o[w++] = s[k]; break; }
            case 'd': { int v = va_arg(ap, int); w = put_num(o, w, cap, v < 0 ? 0u - (uint32_t)v : (uint32_t)v, 10, width, pad, v < 0); break; }
            case 'u': w = put_num(o, w, cap, va_arg(ap, uint32_t), 10, width, pad, false); break;
            case 'x': w = put_num(o, w, cap, va_arg(ap, uint32_t), 16, width, pad, false); break;
            case 'c': { int c = va_arg(ap, int); if (w + 1 < cap) o[w++] = (char)c; break; }
            case '%': if (w + 1 < cap) o[w++] = '%'; break;
            case '\0': if (w + 1 < cap) o[w++] = '%'; fmt--; break;      /* a lone '%' at the end stays */
            default: if (w + 1 < cap) o[w++] = '%'; if (w + 1 < cap) o[w++] = *fmt; break;
        }
    }
    o[w] = '\0';
    return w;
}
void klogf(int level, const char *src, const char *fmt, ...) {
    char buf[LOG_MSG_MAX]; va_list ap;
    va_start(ap, fmt); vformat(buf, sizeof buf, fmt, ap); va_end(ap);
    klog(level, src, buf);
}

/* ---------- tracing ---------- */
void klog_tracef(bool sched, int pid, const char *fmt, ...) {
    char buf[LOG_MSG_MAX]; va_list ap;
    va_start(ap, fmt); vformat(buf, sizeof buf, fmt, ap); va_end(ap);
    if (sched) klog_trace_sched(pid, buf); else klog_trace_syscall(pid, buf);
}
void klog_trace_syscall(int pid, const char *msg) { trace_count++; store(pid, LOG_DEBUG, "trace", msg, true); }
void klog_trace_sched(int pid, const char *msg)   { sched_count++; store(pid, LOG_DEBUG, "sched", msg, true); }
bool klog_syscall_tracing(void) { return trace_on; }
bool klog_sched_tracing(void) { return sched_on; }
int  klog_set_level(int l) { if (l < 0 || l >= LOG_LEVELS) return -1; min_level = (uint8_t)l; return 0; }
int  klog_level(void) { return min_level; }
void klog_set_trace(bool on) { trace_on = on; }
void klog_set_sched_trace(bool on) { sched_on = on; }
void klog_clear(void) {
    uint32_t f = irq_save();
    memset(ring, 0, sizeof ring);
    first_seq = next_seq;                                   /* sequence numbers keep counting: a gap shows that a clear happened */
    overwritten = dropped = 0;
    irq_restore(f);
}

/* ---------- reading ---------- */
bool klog_private_source(const char *src) { return streq_ci(src, "sec") || streq_ci(src, "shell"); }
static uint32_t stored_count(void) { return next_seq - first_seq; }

int klog_get(uint32_t index, bool root, log_entry_t *out) {
    uint32_t f = irq_save(), seen = 0; int result = -1;
    for (uint32_t s = first_seq; s < next_seq; s++) {
        const log_entry_t *e = &ring[s % LOG_CAPACITY];
        if (!root && klog_private_source(e->src)) continue;
        if (out && seen == index) { *out = *e; result = 0; break; }
        seen++;
    }
    if (!out) result = (int)seen;
    irq_restore(f);
    return result;
}
int klog_boot_get(uint32_t index, log_entry_t *out) {
    if (!out) return (int)boot_n;
    if (index >= boot_n) return -1;
    *out = boot_log[index];
    return 0;
}
void klog_boot_done(void) { boot_done = true; }

void klog_stats(bool root, log_stats_t *o) {
    uint32_t f = irq_save();
    memset(o, 0, sizeof *o);
    o->total_written = next_seq - 1; o->stored = stored_count(); o->overwritten = overwritten; o->dropped_below_level = dropped;
    o->capacity = LOG_CAPACITY; o->boot_entries = boot_n; o->min_level = min_level;
    o->trace_syscalls = trace_on; o->trace_sched = sched_on; o->trace_count = trace_count; o->sched_count = sched_count;
    for (uint32_t s = first_seq; s < next_seq; s++) {
        const log_entry_t *e = &ring[s % LOG_CAPACITY];
        if (!root && klog_private_source(e->src)) { o->hidden_private++; continue; }
        o->per_level[e->level]++;
    }
    irq_restore(f);
}

const char *klog_level_name(int l) {
    static const char *const n[LOG_LEVELS] = { "DEBUG", "INFO", "WARN", "ERROR" };
    return (l >= 0 && l < LOG_LEVELS) ? n[l] : "?";
}
int klog_level_from_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < LOG_LEVELS; i++) if (streq_ci(name, klog_level_name(i))) return i;
    if (streq_ci(name, "warning")) return LOG_WARN;
    return -1;
}
size_t klog_format(const log_entry_t *e, char *out, size_t cap) {
    char tmp[16]; size_t w = 0;
    #define EMIT(c) do { char c_ = (c); if (w + 1 < cap) out[w] = c_; w++; } while (0)   /* c is evaluated exactly once, even when the buffer is full */
    uint32_t sec = e->tick / TIMER_HZ, frac = (e->tick % TIMER_HZ) * 100 / TIMER_HZ;
    size_t n = 0; uint32_t v = sec; do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (size_t k = n; k < 6; k++) EMIT(' ');
    while (n) EMIT(tmp[--n]);
    EMIT('.'); EMIT((char)('0' + frac / 10)); EMIT((char)('0' + frac % 10)); EMIT(' ');
    const char *ln = klog_level_name(e->level); size_t l = strlen(ln);
    for (size_t k = 0; k < 5; k++) EMIT(k < l ? ln[k] : ' ');
    EMIT(' ');
    for (size_t k = 0; k < 7; k++) EMIT(k < strlen(e->src) ? e->src[k] : ' ');
    EMIT(' ');
    for (const char *p = e->msg; *p; p++) EMIT(*p);
    if (cap) out[w < cap ? w : cap - 1] = '\0';
    #undef EMIT
    return w;
}
