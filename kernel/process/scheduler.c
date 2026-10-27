/* Round-robin scheduler (single CPU).
 *
 * Switch points:
 *   - timer IRQ  : sched_tick() accounts time and may set need_resched; the interrupt-exit
 *                  hook then calls schedule() AFTER the PIC was acknowledged.
 *   - voluntary  : yield / sleep / wait / suspend / exit call schedule() directly.
 * All queue changes run with interrupts disabled. */
#include "include/logger.h"
#include "include/scheduler.h"
#include "include/arch_context.h"
#include "include/irqflags.h"

static PCB     *current, *idle_task;
static PCB     *ready[PROC_MAX];
static size_t   ready_head, ready_len;
static uint32_t timeslice = SCHED_DEFAULT_SLICE, slice_left = SCHED_DEFAULT_SLICE;
static volatile bool need_resched;
static PCB     *dead_prev;                   /* freed by whoever runs next */
static sched_stats_t st;

void sched_init(void) {
    current = idle_task = dead_prev = NULL;
    ready_head = ready_len = 0;
    timeslice = slice_left = SCHED_DEFAULT_SLICE;
    need_resched = false;
    st = (sched_stats_t){ 0 };
}

void sched_start(PCB *idle, PCB *boot_task) {
    idle_task = idle; current = boot_task;
    boot_task->state = PROC_RUNNING;
    slice_left = timeslice;
}

PCB *sched_current(void) { return current; }
PCB *sched_idle(void)    { return idle_task; }
uint32_t sched_now(void) { return st.total_ticks; }
uint32_t sched_timeslice(void) { return timeslice; }

int sched_set_timeslice(uint32_t ticks) {
    if (ticks < SCHED_MIN_SLICE || ticks > SCHED_MAX_SLICE) return -1;
    timeslice = ticks;
    if (slice_left > ticks) slice_left = ticks;
    return 0;
}

/* ---------- ready queue (circular FIFO) ---------- */
static void enqueue(PCB *p) {
    if (p->queued || ready_len >= PROC_MAX) return;
    ready[(ready_head + ready_len) % PROC_MAX] = p;
    ready_len++; p->queued = true;
}
static PCB *dequeue(void) {
    if (ready_len == 0) return NULL;
    PCB *p = ready[ready_head];
    ready_head = (ready_head + 1) % PROC_MAX; ready_len--;
    p->queued = false;
    return p;
}

void sched_make_ready(PCB *p) {
    uint32_t f = irq_save();
    p->state = PROC_READY; p->wait_kind = WAIT_NONE;
    if (p != idle_task) enqueue(p);
    if (current == idle_task) need_resched = true;       /* real work arrived: leave idle */
    irq_restore(f);
}

void sched_remove(PCB *p) {
    uint32_t f = irq_save();
    if (p->queued) {
        size_t n = ready_len, w = 0;
        for (size_t i = 0; i < n; i++) {
            PCB *q = ready[(ready_head + i) % PROC_MAX];
            if (q != p) ready[(ready_head + w++) % PROC_MAX] = q;
        }
        ready_len = w; p->queued = false;
    }
    irq_restore(f);
}

size_t sched_ready_count(void) { return ready_len; }
const PCB *sched_ready_at(size_t pos) { return pos < ready_len ? ready[(ready_head + pos) % PROC_MAX] : NULL; }

void sched_stats(sched_stats_t *out) { if (out) { *out = st; out->timeslice = timeslice; } }

/* ---------- timer accounting ---------- */
void sched_tick(void) {
    st.total_ticks++;
    if (!current) return;
    current->cpu_ticks++;
    if (current == idle_task) st.idle_ticks++;

    for (size_t i = 0; i < PROC_MAX; i++) {              /* wake sleepers whose time has come */
        PCB *p = process_table_at(i);
        if (p->state == PROC_WAITING && p->wait_kind == WAIT_SLEEP && (int32_t)(st.total_ticks - p->wake_tick) >= 0)
            sched_make_ready(p);
    }
    if (current == idle_task) { if (ready_len) need_resched = true; return; }
    if (slice_left > 0) slice_left--;
    if (slice_left == 0) {
        if (ready_len) need_resched = true;
        else slice_left = timeslice;                     /* nobody else wants the CPU: keep running */
    }
}

static void do_schedule(bool preempted);

void sched_on_irq_exit(void) {
    if (!need_resched) return;
    need_resched = false;
    do_schedule(true);
}

void schedule(void) { do_schedule(false); }

void sched_finish_switch(void) {
    if (dead_prev) { process_release_stack(dead_prev); dead_prev = NULL; }
}

/* ---------- the context switch ---------- */
static void do_schedule(bool preempted) {
    uint32_t flags = irq_save();
    PCB *prev = current;

    /* A task whose stack canary is gone has already corrupted memory: stop it. */
    if (prev->stack_base && prev->state != PROC_TERMINATED &&
        *(uint32_t *)prev->stack_base != PROC_STACK_CANARY) {
        st.stack_overflows++;
        process_kill_internal(prev, PROC_EXIT_STACK_OVERFLOW);
    }

    if (prev->state == PROC_RUNNING) {
        prev->state = PROC_READY;
        if (prev != idle_task) enqueue(prev);
    }
    PCB *next = dequeue();
    if (!next) next = idle_task;
    slice_left = timeslice;
    need_resched = false;

    if (next == prev) {                                  /* nothing better to run */
        prev->state = PROC_RUNNING;
        irq_restore(flags);
        return;
    }

    next->state = PROC_RUNNING;
    next->times_scheduled++;
    current = next;
    st.context_switches++;
    if (preempted) st.preemptions++; else st.voluntary++;
    if (klog_sched_tracing()) klog_tracef(true, prev->pid, "pid %d -> pid %d '%s' (%s)", prev->pid, next->pid, next->name, preempted ? "time slice over" : "yielded or blocked");
    if (prev->state == PROC_TERMINATED) dead_prev = prev;

    arch_context_switch(&prev->esp, next->esp);

    /* ---- we are back: some other task switched to us ---- */
    sched_finish_switch();
    irq_restore(flags);
}
