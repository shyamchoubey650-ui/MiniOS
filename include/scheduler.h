#ifndef SCHEDULER_H
#define SCHEDULER_H
/* Round-robin scheduler with a configurable time slice (in timer ticks).
 * Policy: FIFO ready queue; a running process is preempted when its slice expires and
 * another process is READY. The idle task runs only when nothing else is READY.
 * Priority is NOT used by this policy (documented limitation). */
#include "include/types.h"
#include "include/process.h"

#define SCHED_DEFAULT_SLICE 5      /* 50 ms at 100 Hz */
#define SCHED_MIN_SLICE     1
#define SCHED_MAX_SLICE     100

typedef struct {
    uint32_t context_switches;     /* switches between different tasks */
    uint32_t preemptions;          /* ... forced by an expired time slice */
    uint32_t voluntary;            /* ... caused by yield, sleep, wait, exit, block */
    uint32_t total_ticks;          /* timer ticks since scheduler start */
    uint32_t idle_ticks;           /* ... spent in the idle task */
    uint32_t stack_overflows;      /* tasks killed by a destroyed stack canary */
    uint32_t timeslice;
} sched_stats_t;

void     sched_init(void);
void     sched_start(PCB *idle, PCB *boot_task);   /* boot_task is RUNNING from now on */
int      sched_set_timeslice(uint32_t ticks);       /* 0 or SCHED_OK; invalid -> -1 */
uint32_t sched_timeslice(void);
uint32_t sched_now(void);                           /* scheduler tick counter */

void     sched_tick(void);                          /* timer IRQ hook: accounting, wake sleepers */
void     sched_on_irq_exit(void);                   /* interrupt-exit hook: preempt if needed */
void     schedule(void);                            /* give the CPU away (yield/block/exit) */
void     sched_finish_switch(void);                 /* first thing a resumed/new task does */

void     sched_make_ready(PCB *p);                  /* state -> READY, append to queue */
void     sched_remove(PCB *p);                      /* take out of the queue */
PCB     *sched_current(void);
PCB     *sched_idle(void);
size_t   sched_ready_count(void);
const PCB *sched_ready_at(size_t position);         /* 0 = next to run */
void     sched_stats(sched_stats_t *out);
#endif
