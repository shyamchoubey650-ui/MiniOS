#include "include/workloads.h"
#include "include/logger.h"
#include "include/loopback.h"
#include "include/screen.h"
#include "include/timer.h"
#include "include/scheduler.h"

/* Busy-wait until THIS process has consumed `cpu_ticks` ticks of CPU time. Because the
 * condition counts only the process's own running time, the work takes longer in wall
 * time when other processes compete for the CPU - which is exactly what round robin shows. */
static void burn(uint32_t cpu_ticks) {
    PCB *me = process_current();
    volatile uint32_t *used = &me->cpu_ticks;
    uint32_t target = *used + cpu_ticks;
    volatile uint32_t sink = 0;
    while ((int32_t)(*used - target) < 0) sink++;
}

static void work_demo(void *arg)   { burn((uint32_t)(uintptr_t)arg * TIMER_HZ); }
static void work_spin(void *arg)   { (void)arg; for (;;) burn(TIMER_HZ); }
static void work_sleeper(void *arg){ process_sleep_ticks((uint32_t)(uintptr_t)arg * TIMER_HZ); }

static void work_ticker(void *arg) {
    uint32_t n = (uint32_t)(uintptr_t)arg;
    for (uint32_t i = 1; i <= n; i++) {
        process_sleep_ticks(TIMER_HZ);
        screen_puts("\n[ticker pid "); screen_put_dec((uint32_t)process_current_pid());
        screen_puts("] tick "); screen_put_dec(i); screen_putc('/'); screen_put_dec(n); screen_putc('\n');
    }
}

/* Answers messages sent to loopback port 7777 on port 7778 ("echo: <text>") for N seconds, or until it receives "quit". */
static void work_lbecho(void *arg) {
    uint32_t deadline = timer_ticks() + (uint32_t)(uintptr_t)arg * TIMER_HZ;
    while ((int32_t)(timer_ticks() - deadline) < 0) {
        int r = lo_echo_step();
        if (r == 2) return;
        if (r == 0) process_sleep_ticks(TIMER_HZ / 10);
    }
}

static void work_crash(void *arg) {
    (void)arg;
    burn(1);
    uint32_t zero = 0;
    /* A REAL divide-by-zero -> CPU exception #0 -> only this process dies. It must be written in assembly: in plain C,
     * `1 / zero` is undefined behaviour and GCC -O2 compiles it to a compare with no divide instruction, so nothing ever faulted. */
    __asm__ volatile ("xorl %%edx, %%edx\n\tmovl $1, %%eax\n\tdivl %0" : : "r"(zero) : "eax", "edx", "cc");
}

static const workload_t workloads[] = {
    { "demo",    "burns CPU for N seconds of its own run time", "seconds (1-600)", 3, work_demo    },
    { "spin",    "burns CPU forever (use kill to stop it)",     NULL,              0, work_spin    },
    { "sleeper", "sleeps N seconds in the WAITING state",       "seconds (1-600)", 5, work_sleeper },
    { "ticker",  "prints one line per second, N times",         "count (1-600)",   5, work_ticker  },
    { "crash",   "divides by zero; the kernel kills only it",   NULL,              0, work_crash   },
    { "lbecho",  "echoes loopback port 7777 to 7778 for N seconds", "seconds (1-600)", 30, work_lbecho },
};

size_t workload_count(void) { return sizeof(workloads) / sizeof(workloads[0]); }
const workload_t *workload_at(size_t i) { return i < workload_count() ? &workloads[i] : NULL; }

const workload_t *workload_find(const char *kind) {
    if (!kind) return NULL;
    for (size_t i = 0; i < workload_count(); i++) {
        const char *a = workloads[i].kind, *b = kind;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) return &workloads[i];
    }
    return NULL;
}

int workload_spawn(const workload_t *w, const char *name, uint32_t arg, int priority, int *pid_out) {
    if (!w) return PROC_ERR_ARG;
    return process_create(name ? name : w->kind, w->fn, (void *)(uintptr_t)arg, priority, pid_out);
}

bool process_fault_hook(registers_t *r) {
    PCB *me = process_current();
    if (!me || me->pid <= PID_SHELL) return false;             /* the shell/idle crashing is a real kernel panic */
    klogf(LOG_ERROR, "cpu", "%s in pid %d '%s' at eip=%x: process terminated", exception_name(r->int_no), me->pid, me->name, r->eip);
    screen_puts("\n[kernel] process "); screen_put_dec((uint32_t)me->pid);
    screen_puts(" '"); screen_puts(me->name); screen_puts("' terminated: ");
    screen_puts(exception_name(r->int_no)); screen_puts(" at EIP="); screen_put_hex(r->eip); screen_putc('\n');
    process_exit(PROC_EXIT_FAULT_BASE + (int)r->int_no);
}
