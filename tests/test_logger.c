/* Unit tests for kernel/log/logger.c (ring buffer, levels, privacy, boot snapshot, trace bypass, formatter). */
#include <stdio.h>
#include <string.h>
#include "include/logger.h"

static int run, failed;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; printf("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static uint32_t fake = 0; uint32_t timer_ticks(void) { return fake; }
static int cur = 7; static int pid_src(void) { return cur; }

static log_entry_t e;
static bool nth(uint32_t i, bool root) { return klog_get(i, root, &e) == 0; }

static void test_basic(void) {
    puts("basic"); klog_init(); klog_set_pid_source(NULL);
    CHECK(klog_get(0, true, NULL) == 0 && klog_get(0, true, &e) == -1);
    fake = 150; klog(LOG_INFO, "boot", "hello");
    CHECK(klog_get(0, true, NULL) == 1 && nth(0, true) && e.seq == 1 && e.tick == 150 && e.pid == -1 && e.level == LOG_INFO && !strcmp(e.src, "boot") && !strcmp(e.msg, "hello"));
    klog(LOG_ERROR, "cpu", "second"); klog(LOG_WARN, "fs", "third");
    CHECK(klog_get(0, true, NULL) == 3 && nth(1, true) && !strcmp(e.msg, "second") && nth(2, true) && e.seq == 3 && klog_get(3, true, &e) == -1);
    klog_set_pid_source(pid_src); klog(LOG_INFO, "proc", "with pid"); nth(3, true); CHECK(e.pid == 7);
    klog_pid(42, LOG_INFO, "proc", "explicit"); nth(4, true); CHECK(e.pid == 42);
    klog_set_pid_source(NULL);
}

static void test_sanitise(void) {
    puts("sanitising"); klog_init();
    klog(LOG_INFO, "a\nb", "line1\nline2\tx\x1b[31m"); nth(0, true);
    CHECK(!strchr(e.msg, '\n') && !strchr(e.msg, '\t') && !strchr(e.msg, 0x1b) && !strchr(e.src, '\n') && !strcmp(e.msg, "line1?line2?x?[31m"));
    char longm[200]; memset(longm, 'x', 199); longm[199] = 0; klog(LOG_INFO, "averyveryverylongsource", longm); nth(1, true);
    CHECK(strlen(e.msg) == LOG_MSG_MAX - 1 && strlen(e.src) == LOG_SRC_MAX - 1);
    klog(LOG_INFO, NULL, NULL); nth(2, true); CHECK(e.msg[0] == 0 && e.src[0] == 0);
    klog(99, "x", "bad level"); nth(3, true); CHECK(e.level == LOG_ERROR); klog(-5, "x", "neg level"); CHECK(klog_get(0, true, NULL) == 4 || klog_get(0, true, NULL) == 3);
}

static void test_formatter(void) {
    puts("klogf"); klog_init(); log_entry_t g;
    #define LAST(str) do { int n = klog_get(0, true, NULL); klog_get((uint32_t)n - 1, true, &g); CHECK(!strcmp(g.msg, str)); if (strcmp(g.msg, str)) printf("    got [%s]\n", g.msg); } while (0)
    klogf(LOG_INFO, "t", "pid %d name %s", 5, "abc"); LAST("pid 5 name abc");
    klogf(LOG_INFO, "t", "neg %d zero %d", -17, 0); LAST("neg -17 zero 0");
    klogf(LOG_INFO, "t", "min %d", -2147483647 - 1); LAST("min -2147483648");
    klogf(LOG_INFO, "t", "u %u hex %x", 4294967295u, 0xBEEFu); LAST("u 4294967295 hex beef");
    klogf(LOG_INFO, "t", "[%5d] [%05d] [%08x] [%3s]", 42, 42, 0xAB, "x"); LAST("[   42] [00042] [000000ab] [  x]");
    klogf(LOG_INFO, "t", "100%% %c%c", 'o', 'k'); LAST("100% ok");
    klogf(LOG_INFO, "t", "null %s", (const char *)0); LAST("null (null)");
    klogf(LOG_INFO, "t", "unknown %q and trailing %"); LAST("unknown %q and trailing %");
    klogf(LOG_INFO, "t", "%s", "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"); LAST("01234567890123456789012345678901234567890123456789012345678901234567890");
    klogf(LOG_INFO, "t", "%d", 0); LAST("0");
}

static void test_ring(void) {
    puts("ring and wrap"); klog_init(); log_stats_t s;
    for (int i = 1; i <= 300; i++) klogf(LOG_INFO, "t", "m%d", i);
    klog_stats(true, &s);
    CHECK(s.total_written == 300 && s.stored == LOG_CAPACITY && s.overwritten == 44 && s.capacity == LOG_CAPACITY);
    CHECK(klog_get(0, true, NULL) == LOG_CAPACITY && nth(0, true) && e.seq == 45 && !strcmp(e.msg, "m45") && nth(LOG_CAPACITY - 1, true) && e.seq == 300 && !strcmp(e.msg, "m300"));
    bool ordered = true; uint32_t prev = 0; for (uint32_t i = 0; i < LOG_CAPACITY; i++) { klog_get(i, true, &e); if (e.seq != prev + 1 && prev) ordered = false; prev = e.seq; } CHECK(ordered);
    klog_clear(); klog_stats(true, &s);
    CHECK(s.stored == 0 && s.overwritten == 0 && s.total_written == 300 && klog_get(0, true, NULL) == 0);          /* the counter keeps going */
    klog(LOG_INFO, "t", "after clear"); nth(0, true); CHECK(e.seq == 301);
}

static void test_levels(void) {
    puts("levels"); klog_init(); log_stats_t s;
    CHECK(klog_level() == LOG_INFO);
    klog(LOG_DEBUG, "t", "hidden debug"); klog(LOG_INFO, "t", "i"); klog(LOG_WARN, "t", "w"); klog(LOG_ERROR, "t", "e");
    klog_stats(true, &s); CHECK(s.stored == 3 && s.dropped_below_level == 1 && s.per_level[LOG_DEBUG] == 0 && s.per_level[LOG_INFO] == 1 && s.per_level[LOG_WARN] == 1 && s.per_level[LOG_ERROR] == 1);
    CHECK(klog_set_level(LOG_ERROR) == 0 && klog_level() == LOG_ERROR); klog(LOG_WARN, "t", "dropped"); klog(LOG_ERROR, "t", "kept");
    klog_stats(true, &s); CHECK(s.stored == 4 && s.dropped_below_level == 2 && s.min_level == LOG_ERROR);
    CHECK(klog_set_level(-1) == -1 && klog_set_level(LOG_LEVELS) == -1 && klog_level() == LOG_ERROR);
    klog_set_level(LOG_DEBUG); klog(LOG_DEBUG, "t", "now stored"); klog_stats(true, &s); CHECK(s.per_level[LOG_DEBUG] == 1);
    CHECK(!strcmp(klog_level_name(LOG_WARN), "WARN") && !strcmp(klog_level_name(9), "?") && klog_level_from_name("error") == LOG_ERROR && klog_level_from_name("WARNING") == LOG_WARN && klog_level_from_name("Debug") == LOG_DEBUG);
    CHECK(klog_level_from_name("nope") == -1 && klog_level_from_name("") == -1 && klog_level_from_name(NULL) == -1 && klog_level_from_name("inf") == -1);
}

static void test_trace(void) {
    puts("trace bypasses the level"); klog_init(); log_stats_t s;
    klog_set_level(LOG_ERROR); CHECK(!klog_syscall_tracing() && !klog_sched_tracing());
    klog_set_trace(true); klog_set_sched_trace(true); CHECK(klog_syscall_tracing() && klog_sched_tracing());
    klog_trace_syscall(3, "open(...) = 4"); klog_trace_sched(1, "switch 1 -> 3");
    klog_stats(true, &s); CHECK(s.stored == 2 && s.trace_count == 1 && s.sched_count == 1 && s.trace_syscalls && s.trace_sched && s.dropped_below_level == 0);
    nth(0, true); CHECK(!strcmp(e.src, "trace") && e.pid == 3 && e.level == LOG_DEBUG); nth(1, true); CHECK(!strcmp(e.src, "sched") && e.pid == 1);
    klog_set_trace(false); klog_set_sched_trace(false); CHECK(!klog_syscall_tracing() && !klog_sched_tracing());
}

static void test_privacy(void) {
    puts("private sources"); klog_init(); log_stats_t s;
    klog(LOG_INFO, "boot", "public 1"); klog(LOG_WARN, "sec", "login failed"); klog(LOG_INFO, "SEC", "upper case source"); klog(LOG_DEBUG, "shell", "x");
    klog(LOG_INFO, "proc", "public 2");
    CHECK(klog_private_source("sec") && klog_private_source("shell") && klog_private_source("SEC") && !klog_private_source("proc") && !klog_private_source("security") && !klog_private_source(""));
    CHECK(klog_get(0, true, NULL) == 4);                                                   /* shell/DEBUG was below the level: 4 stored */
    CHECK(klog_get(0, false, NULL) == 2 && nth(0, false) && !strcmp(e.msg, "public 1") && nth(1, false) && !strcmp(e.msg, "public 2") && klog_get(2, false, &e) == -1);
    klog_stats(false, &s); CHECK(s.hidden_private == 2 && s.stored == 4 && s.per_level[LOG_WARN] == 0 && s.per_level[LOG_INFO] == 2);
    klog_stats(true, &s); CHECK(s.hidden_private == 0 && s.per_level[LOG_WARN] == 1);
    CHECK(nth(1, true) && !strcmp(e.src, "sec"));
}

static void test_boot(void) {
    puts("boot snapshot"); klog_init(); log_entry_t b;
    CHECK(klog_boot_get(0, NULL) == 0 && klog_boot_get(0, &b) == -1);
    for (int i = 0; i < 10; i++) klogf(LOG_INFO, "boot", "step %d", i);
    klog_boot_done();
    for (int i = 0; i < 400; i++) klogf(LOG_INFO, "proc", "later %d", i);                  /* wraps the ring many times */
    CHECK(klog_boot_get(0, NULL) == 10 && klog_boot_get(0, &b) == 0 && !strcmp(b.msg, "step 0") && klog_boot_get(9, &b) == 0 && !strcmp(b.msg, "step 9") && klog_boot_get(10, &b) == -1);
    CHECK(nth(0, true) && strcmp(e.src, "boot") != 0);                                    /* the ring forgot boot; the snapshot did not */
    klog_init(); for (int i = 0; i < LOG_BOOT_KEEP + 20; i++) klog(LOG_INFO, "boot", "x");
    CHECK(klog_boot_get(0, NULL) == LOG_BOOT_KEEP);
    klog_init(); CHECK(klog_boot_get(0, NULL) == 0);
}

static void test_format(void) {
    puts("klog_format"); klog_init(); char b[100]; fake = 12345; klog(LOG_WARN, "proc", "created pid 4"); nth(0, true);
    size_t n = klog_format(&e, b, sizeof b); CHECK(n == strlen(b) && !strcmp(b, "   123.45 WARN  proc    created pid 4"));
    fake = 5; klog(LOG_ERROR, "cpu", "x"); nth(1, true); klog_format(&e, b, sizeof b); CHECK(!strcmp(b, "     0.05 ERROR cpu     x"));
    char tiny[8]; n = klog_format(&e, tiny, sizeof tiny); CHECK(strlen(tiny) == 7 && n > 7);                // truncated, still terminated, full length reported
    CHECK(klog_format(&e, tiny, 0) > 0);
}

int main(void) {
    test_basic(); test_sanitise(); test_formatter(); test_ring(); test_levels(); test_trace(); test_privacy(); test_boot(); test_format();
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
