/* Process lifecycle: create, exit, kill, wait, suspend/resume, sleep.
 *
 * State machine:
 *   NEW -> READY <-> RUNNING -> TERMINATED -> (slot freed by wait)
 *                \-> WAITING (sleep | wait for child | suspended) -> READY
 * A terminated process stays in the table as a "zombie" holding its exit code until
 * its parent calls wait(); its stack is freed as soon as it is no longer running. */
#include "include/logger.h"
#include "include/process.h"
#include "include/scheduler.h"
#include "include/arch_context.h"
#include "include/irqflags.h"
#include "include/memory.h"
#include "include/string.h"

static PCB table[PROC_MAX];
static void (*cleanup_hook)(PCB *);
void process_set_cleanup_hook(void (*fn)(PCB *p)) { cleanup_hook = fn; }
static int next_pid;

PCB *process_table_at(size_t i) { return &table[i % PROC_MAX]; }
PCB *process_current(void) { return sched_current(); }
int  process_current_pid(void) { PCB *c = sched_current(); return c ? c->pid : -1; }

static PCB *find(int pid) {
    if (pid < 0) return NULL;
    for (size_t i = 0; i < PROC_MAX; i++)
        if (table[i].state != PROC_UNUSED && table[i].pid == pid) return &table[i];
    return NULL;
}
const PCB *process_get(int pid) { return find(pid); }
const PCB *process_slot(size_t i) { return (i < PROC_MAX && table[i].state != PROC_UNUSED) ? &table[i] : NULL; }

int process_count_state(proc_state_t s) {
    int n = 0;
    for (size_t i = 0; i < PROC_MAX; i++) if (table[i].state == (int)s) n++;
    return n;
}

static void copy_name(char *dst, const char *src) {
    size_t i = 0;
    while (src[i] && i < PROC_NAME_MAX - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Free slot, or reclaim the oldest zombie (its exit status is lost). NULL if all slots are live. */
static PCB *alloc_slot(void) {
    PCB *oldest = NULL;
    for (size_t i = 0; i < PROC_MAX; i++) {
        if (table[i].state == PROC_UNUSED) return &table[i];
        if (table[i].state == PROC_TERMINATED && (!oldest || (int32_t)(oldest->exit_tick - table[i].exit_tick) > 0)) oldest = &table[i];
    }
    return oldest;
}

void process_release_stack(PCB *p) {
    if (p->stack_base) { kfree(p->stack_base); p->stack_base = NULL; }
    p->memory_used = 0;
}

static void idle_entry(void *arg) { (void)arg; for (;;) arch_idle(); }

void process_trampoline(void) {
    sched_finish_switch();            /* free whoever we replaced if it had died */
    irq_enable();                     /* we were entered from inside an interrupt: IF is still 0 */
    PCB *me = sched_current();
    me->entry(me->arg);
    process_exit(0);
}

static void init_pcb(PCB *p, int pid, const char *name, int prio, int parent) {
    memset(p, 0, sizeof *p);
    p->pid = pid; copy_name(p->name, name); p->priority = prio; p->parent_pid = parent;
    p->state = PROC_NEW; p->wait_kind = WAIT_NONE; p->wait_pid = -1;
    p->start_tick = sched_now();
}

void process_init(void) {
    memset(table, 0, sizeof table);
    sched_init();
    next_pid = 2;

    PCB *idle = &table[0];
    init_pcb(idle, PID_IDLE, "idle", PRIORITY_MIN, PID_IDLE);
    idle->stack_base = kmalloc(PROC_STACK_SIZE);
    idle->stack_size = PROC_STACK_SIZE;
    idle->memory_used = idle->stack_base ? (int)PROC_STACK_SIZE : 0;
    idle->entry = idle_entry;
    if (idle->stack_base) {
        *(uint32_t *)idle->stack_base = PROC_STACK_CANARY;
        idle->esp = arch_stack_init(idle->stack_base + PROC_STACK_SIZE, process_trampoline);
    }
    idle->state = PROC_READY;

    PCB *sh = &table[1];                                   /* the code that is running right now */
    init_pcb(sh, PID_SHELL, "shell", PRIORITY_DEFAULT, PID_IDLE);
    sh->stack_size = PROC_BOOT_STACK; sh->memory_used = (int)PROC_BOOT_STACK;   /* boot stack, in the kernel image */
    sched_start(idle, sh);
}

int process_create(const char *name, proc_entry_t fn, void *arg, int priority, int *pid_out) {
    if (!name || !*name || !fn || priority < PRIORITY_MIN || priority > PRIORITY_MAX) return PROC_ERR_ARG;
    if (next_pid < 0) return PROC_ERR_NO_SLOT;             /* pid counter exhausted */

    uint32_t f = irq_save();
    uint8_t *stack = kmalloc(PROC_STACK_SIZE);
    if (!stack) { irq_restore(f); return PROC_ERR_NO_MEMORY; }
    PCB *p = alloc_slot();
    if (!p) { kfree(stack); irq_restore(f); return PROC_ERR_NO_SLOT; }

    int parent = process_current_pid();
    init_pcb(p, next_pid++, name, priority, parent);
    p->stack_base = stack; p->stack_size = PROC_STACK_SIZE; p->memory_used = (int)PROC_STACK_SIZE;
    p->entry = fn; p->arg = arg;
    *(uint32_t *)stack = PROC_STACK_CANARY;
    p->esp = arch_stack_init(stack + PROC_STACK_SIZE, process_trampoline);
    if (pid_out) *pid_out = p->pid;
    sched_make_ready(p);
    klogf(LOG_INFO, "proc", "created pid %d '%s' (parent %d, priority %d)", p->pid, p->name, parent, priority);
    irq_restore(f);
    return PROC_OK;
}

/* Terminate p (any state except TERMINATED). Does NOT switch away. */
void process_kill_internal(PCB *p, int code) {
    uint32_t f = irq_save();
    if (p->state == PROC_TERMINATED || p->state == PROC_UNUSED) { irq_restore(f); return; }
    if (p->queued) sched_remove(p);
    p->state = PROC_TERMINATED; p->wait_kind = WAIT_NONE;
    p->exit_code = code; p->exit_tick = sched_now();
    if (code == PROC_EXIT_KILLED) klogf(LOG_INFO, "proc", "pid %d '%s' killed", p->pid, p->name);
    else if (code == PROC_EXIT_STACK_OVERFLOW) klogf(LOG_ERROR, "proc", "pid %d '%s' stopped: stack overflow", p->pid, p->name);
    else if (code >= PROC_EXIT_FAULT_BASE && code < PROC_EXIT_FAULT_BASE + 32) klogf(LOG_ERROR, "proc", "pid %d '%s' died: CPU exception %d", p->pid, p->name, code - PROC_EXIT_FAULT_BASE);
    else klogf(LOG_INFO, "proc", "pid %d '%s' exited with code %d", p->pid, p->name, code);
    if (cleanup_hook) cleanup_hook(p);                     /* close files, free the process's allocations */

    for (size_t i = 0; i < PROC_MAX; i++)                  /* orphans are adopted by the shell */
        if (table[i].state != PROC_UNUSED && table[i].parent_pid == p->pid && &table[i] != p) table[i].parent_pid = PID_SHELL;

    PCB *parent = find(p->parent_pid);
    if (parent && parent != p && parent->state == PROC_WAITING && parent->wait_kind == WAIT_CHILD &&
        (parent->wait_pid == -1 || parent->wait_pid == p->pid))
        sched_make_ready(parent);

    if (p != sched_current()) process_release_stack(p);    /* a running task's stack is freed after the switch */
    irq_restore(f);
}

void process_exit(int code) {
    PCB *me = sched_current();
    if (me->pid > PID_SHELL) {
        process_kill_internal(me, code);
        schedule();
    }
    for (;;) arch_idle();                                  /* idle/shell must never exit */
}

int process_kill(int pid) {
    PCB *p = find(pid);
    if (!p) return PROC_ERR_NO_SUCH_PID;
    if (p->pid <= PID_SHELL) return PROC_ERR_PROTECTED;
    if (p->state == PROC_TERMINATED) return PROC_ERR_BAD_STATE;
    if (p == sched_current()) process_exit(PROC_EXIT_KILLED);   /* does not return */
    process_kill_internal(p, PROC_EXIT_KILLED);
    return PROC_OK;
}

/* A terminated child matching pid (or any child if pid == -1), or NULL. */
static PCB *find_zombie(int me, int pid) {
    for (size_t i = 0; i < PROC_MAX; i++)
        if (table[i].state == PROC_TERMINATED && table[i].parent_pid == me && (pid == -1 || table[i].pid == pid)) return &table[i];
    return NULL;
}
static bool has_child(int me, int pid) {
    for (size_t i = 0; i < PROC_MAX; i++)
        if (table[i].state != PROC_UNUSED && table[i].parent_pid == me && table[i].pid != me && (pid == -1 || table[i].pid == pid)) return true;
    return false;
}

int process_wait(int pid, int *exit_code) {
    PCB *me = sched_current();
    if (pid == me->pid) return PROC_ERR_SELF_WAIT;
    if (pid != -1) {
        PCB *t = find(pid);
        if (!t) return PROC_ERR_NO_SUCH_PID;
        if (t->parent_pid != me->pid) return PROC_ERR_NOT_CHILD;
    }
    for (;;) {
        uint32_t f = irq_save();
        PCB *z = find_zombie(me->pid, pid);
        if (z) {
            int reaped = z->pid;
            if (exit_code) *exit_code = z->exit_code;
            process_release_stack(z);
            z->state = PROC_UNUSED;                         /* slot is free again */
            irq_restore(f);
            return reaped;
        }
        if (!has_child(me->pid, pid)) { irq_restore(f); return PROC_ERR_NO_CHILD; }
        if (me->interrupted) { me->interrupted = false; irq_restore(f); return PROC_ERR_INTERRUPTED; }
        me->state = PROC_WAITING; me->wait_kind = WAIT_CHILD; me->wait_pid = pid;
        schedule();                                         /* returns when a child exits or Ctrl+C */
        irq_restore(f);
    }
}

int process_suspend(int pid) {
    PCB *p = find(pid);
    if (!p) return PROC_ERR_NO_SUCH_PID;
    if (p->pid <= PID_SHELL) return PROC_ERR_PROTECTED;
    if (p->state != PROC_READY && p->state != PROC_RUNNING) return PROC_ERR_BAD_STATE;
    uint32_t f = irq_save();
    if (p->queued) sched_remove(p);
    p->state = PROC_WAITING; p->wait_kind = WAIT_SUSPENDED;
    if (p == sched_current()) schedule();
    irq_restore(f);
    return PROC_OK;
}

int process_resume(int pid) {
    PCB *p = find(pid);
    if (!p) return PROC_ERR_NO_SUCH_PID;
    if (p->state != PROC_WAITING || p->wait_kind != WAIT_SUSPENDED) return PROC_ERR_BAD_STATE;
    sched_make_ready(p);
    return PROC_OK;
}

int process_set_priority(int pid, int priority) {
    if (priority < PRIORITY_MIN || priority > PRIORITY_MAX) return PROC_ERR_ARG;
    PCB *p = find(pid);
    if (!p) return PROC_ERR_NO_SUCH_PID;
    if (p->pid == PID_IDLE) return PROC_ERR_PROTECTED;
    if (p->state == PROC_TERMINATED) return PROC_ERR_BAD_STATE;
    p->priority = priority;
    return PROC_OK;
}

void process_yield(void) { schedule(); }

void process_sleep_ticks(uint32_t ticks) {
    PCB *me = sched_current();
    if (me->pid == PID_IDLE || ticks == 0) { process_yield(); return; }       /* idle must never sleep */
    uint32_t f = irq_save();
    me->wake_tick = sched_now() + ticks;
    me->state = PROC_WAITING; me->wait_kind = WAIT_SLEEP;
    schedule();
    irq_restore(f);
}

bool process_ctrl_c(void) {                                /* called from the keyboard IRQ */
    PCB *sh = find(PID_SHELL);
    if (sh && sh->state == PROC_WAITING && sh->wait_kind == WAIT_CHILD) {
        sh->interrupted = true;
        sched_make_ready(sh);
        return true;                                       /* key consumed */
    }
    return false;
}

const char *process_state_name(int s) {
    switch (s) {
        case PROC_UNUSED: return "UNUSED";   case PROC_NEW: return "NEW";
        case PROC_READY: return "READY";     case PROC_RUNNING: return "RUNNING";
        case PROC_WAITING: return "WAITING"; case PROC_TERMINATED: return "TERMINATED";
        default: return "?";
    }
}

const char *process_strerror(int c) {
    switch (c) {
        case PROC_OK:               return "ok";
        case PROC_ERR_ARG:          return "invalid argument";
        case PROC_ERR_NO_SLOT:      return "process table is full";
        case PROC_ERR_NO_MEMORY:    return "out of kernel memory for a process stack";
        case PROC_ERR_NO_SUCH_PID:  return "no such process";
        case PROC_ERR_PROTECTED:    return "process is protected (idle/shell)";
        case PROC_ERR_NOT_CHILD:    return "not a child of this process";
        case PROC_ERR_NO_CHILD:     return "no child processes";
        case PROC_ERR_INTERRUPTED:  return "interrupted";
        case PROC_ERR_BAD_STATE:    return "process is not in a suitable state";
        case PROC_ERR_SELF_WAIT:    return "a process cannot wait for itself";
        default:                    return "unknown process error";
    }
}
