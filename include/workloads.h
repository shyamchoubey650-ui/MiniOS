#ifndef WORKLOADS_H
#define WORKLOADS_H
/* Built-in demo programs for `spawn`. MiniOS has no program loader or user mode yet, so
 * "programs" are kernel functions run as real scheduled tasks (REAL tasks, SIMULATED work). */
#include "include/process.h"
#include "include/interrupts.h"

typedef struct {
    const char  *kind;
    const char  *help;
    const char  *arg_help;      /* NULL = takes no argument */
    uint32_t     default_arg;
    proc_entry_t fn;
} workload_t;

size_t            workload_count(void);
const workload_t *workload_at(size_t index);
const workload_t *workload_find(const char *kind);
int               workload_spawn(const workload_t *w, const char *name, uint32_t arg, int priority, int *pid_out);

/* Exception hook: terminates a faulting process instead of panicking the kernel. */
bool process_fault_hook(registers_t *regs);
#endif
