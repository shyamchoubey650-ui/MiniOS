#include "include/arch_context.h"

uint32_t arch_stack_init(uint8_t *stack_top, void (*entry)(void)) {
    /* The i386 ABI wants esp+4 to be 16-byte aligned at function entry. */
    uint32_t *sp = (uint32_t *)((uintptr_t)stack_top & ~(uintptr_t)15);
    *--sp = 0;                                  /* fake return address for entry() (never used) */
    *--sp = (uint32_t)(uintptr_t)entry;         /* popped by the `ret` in arch_context_switch    */
    *--sp = 0;                                  /* ebp */
    *--sp = 0;                                  /* ebx */
    *--sp = 0;                                  /* esi */
    *--sp = 0;                                  /* edi  <- initial saved esp */
    return (uint32_t)(uintptr_t)sp;
}

void arch_idle(void) { __asm__ volatile ("hlt"); }
