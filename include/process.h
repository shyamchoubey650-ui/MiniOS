#ifndef PROCESS_H
#define PROCESS_H
/* Process manager. Each process is a KERNEL task: its own 8 KiB stack and register
 * context, all running in ring 0 and sharing one address space (no memory
 * protection yet - see docs/PROCESS_MANAGEMENT.md). Context switches are REAL. */
#include "include/types.h"

#define PROC_MAX            16        /* table slots, including idle and the shell */
#define PROC_NAME_MAX       32
#define PROC_STACK_SIZE     8192u
#define PROC_BOOT_STACK     16384u    /* boot stack used by the shell; must match boot/boot.S */
#define PROC_STACK_CANARY   0xC0FFEE42u

#define PID_IDLE            0
#define PID_SHELL           1
#define PRIORITY_MIN        1
#define PRIORITY_MAX        20
#define PRIORITY_DEFAULT    10

/* Exit codes follow the shell convention 128 + signal-like number. */
#define PROC_EXIT_KILLED          137   /* killed by kill/pkill            */
#define PROC_EXIT_STACK_OVERFLOW  139   /* stack canary destroyed          */
#define PROC_EXIT_FAULT_BASE      128   /* CPU exception: 128 + vector     */

typedef enum {
    PROC_UNUSED = 0,   /* free table slot */
    PROC_NEW,          /* being created (momentary) */
    PROC_READY,        /* in the ready queue */
    PROC_RUNNING,      /* owns the CPU */
    PROC_WAITING,      /* sleeping, suspended, or waiting for a child */
    PROC_TERMINATED    /* finished; slot kept until the parent calls wait */
} proc_state_t;

typedef enum { WAIT_NONE = 0, WAIT_CHILD, WAIT_SLEEP, WAIT_SUSPENDED } wait_kind_t;

typedef void (*proc_entry_t)(void *arg);

typedef struct {
    /* the Process Control Block from the project specification */
    int          pid;
    char         name[PROC_NAME_MAX];
    int          state;               /* proc_state_t */
    int          priority;            /* recorded and shown; the round-robin scheduler ignores it */
    int          memory_used;         /* bytes of kernel memory owned (its stack) */
    int          parent_pid;
    /* scheduling and bookkeeping */
    int          exit_code;
    int          wait_kind;           /* wait_kind_t */
    int          wait_pid;            /* WAIT_CHILD: pid waited for, -1 = any child */
    uint32_t     wake_tick;           /* WAIT_SLEEP */
    uint32_t     cpu_ticks;           /* timer ticks spent running */
    uint32_t     start_tick, exit_tick;
    uint32_t     times_scheduled;
    bool         queued;              /* currently in the ready queue */
    bool         interrupted;         /* Ctrl+C while waiting */
    /* machine context */
    uint32_t     esp;                 /* saved stack pointer while not running */
    uint8_t     *stack_base;          /* NULL for idle-less boot stack */
    size_t       stack_size;
    proc_entry_t entry;
    void        *arg;
    void        *res;                 /* open files and allocations (kernel/syscall/syscall.c); freed on exit */
} PCB;

#define PROC_OK                 0
#define PROC_ERR_ARG          (-1)
#define PROC_ERR_NO_SLOT      (-2)   /* process table full of live processes */
#define PROC_ERR_NO_MEMORY    (-3)   /* kernel heap cannot supply a stack    */
#define PROC_ERR_NO_SUCH_PID  (-4)
#define PROC_ERR_PROTECTED    (-5)   /* idle and the shell cannot be killed/suspended */
#define PROC_ERR_NOT_CHILD    (-6)
#define PROC_ERR_NO_CHILD     (-7)   /* wait with no children */
#define PROC_ERR_INTERRUPTED  (-8)   /* wait aborted with Ctrl+C */
#define PROC_ERR_BAD_STATE    (-9)   /* e.g. killing an already terminated process */
#define PROC_ERR_SELF_WAIT   (-10)

/* Called whenever a process terminates (exit, kill, fault), before its slot can be reused. */
void process_set_cleanup_hook(void (*fn)(PCB *p));
void process_init(void);     /* creates idle (PID 0) and adopts the running code as the shell (PID 1) */
int  process_create(const char *name, proc_entry_t fn, void *arg, int priority, int *pid_out);
void process_exit(int code) __attribute__((noreturn));
int  process_kill(int pid);
int  process_wait(int pid, int *exit_code);          /* pid -1 = any child; returns reaped pid or PROC_ERR_* */
int  process_suspend(int pid);
int  process_resume(int pid);
int  process_set_priority(int pid, int priority);
void process_yield(void);
void process_sleep_ticks(uint32_t ticks);

PCB       *process_current(void);
int        process_current_pid(void);
const PCB *process_get(int pid);                      /* NULL if there is no such process */
const PCB *process_slot(size_t index);                /* NULL if the slot is unused; index < PROC_MAX */
int        process_count_state(proc_state_t state);

const char *process_state_name(int state);
const char *process_strerror(int code);

/* hooks and internals */
bool process_ctrl_c(void);                            /* keyboard hook: wakes a shell blocked in wait */
void process_trampoline(void);                        /* first code every new task runs */
void process_release_stack(PCB *p);                   /* scheduler: free a dead task's stack after switching away */
void process_kill_internal(PCB *p, int code);         /* terminate without permission checks (scheduler, fault hook) */
PCB *process_table_at(size_t index);                  /* scheduler: mutable access */
#endif
