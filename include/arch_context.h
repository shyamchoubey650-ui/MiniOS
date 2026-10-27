#ifndef ARCH_CONTEXT_H
#define ARCH_CONTEXT_H
#include "include/types.h"
/* The only CPU-specific part of the scheduler (implemented in boot/context.S and
 * kernel/cpu/context.c; replaced by stubs in host-side tests). */

/* Saves callee-saved registers on the current stack, stores the stack pointer in
 * *old_esp, loads new_esp and restores the other context's registers. Returns
 * (much later) when someone switches back to the saved context. */
void arch_context_switch(uint32_t *old_esp, uint32_t new_esp);

/* Builds the first stack frame of a new task so that the first switch to it
 * "returns" into entry(). `stack_top` is one past the highest stack byte.
 * Returns the initial saved esp. entry() must never return. */
uint32_t arch_stack_init(uint8_t *stack_top, void (*entry)(void));

void arch_idle(void);                 /* halt until the next interrupt */
#endif
