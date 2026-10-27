#ifndef INTERRUPTS_H
#define INTERRUPTS_H
#include "include/types.h"

#define IRQ_BASE   32          /* IRQ0 -> vector 32 after PIC remap */
#define SYSCALL_VECTOR 0x80   /* int 0x80 */
#define IRQ_TIMER  0
#define IRQ_KEYBOARD 1

/* Register frame built by boot/interrupts.S (order matters!). */
typedef struct {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
} registers_t;

typedef void (*interrupt_handler_t)(registers_t *regs);

void interrupts_init(void);                 /* PIC remap + IDT/GDT hookup  */
void interrupt_register(uint32_t vector, interrupt_handler_t h);
void interrupts_enable(void);
void interrupts_disable(void);
void irq_unmask(uint8_t irq);
void irq_mask(uint8_t irq);
const char *exception_name(uint32_t vector);
uint32_t interrupt_irq_count(unsigned irq);   /* hardware interrupts delivered so far */
bool     interrupt_irq_masked(unsigned irq);  /* PIC mask bit */

/* Called at the very end of every hardware IRQ, after the PIC was acknowledged.
 * The scheduler uses it to preempt the interrupted task. */
void interrupts_set_exit_hook(void (*fn)(void));
/* Called for an exception nobody registered a handler for. Return true if it was
 * handled (e.g. the faulting process was terminated); false = kernel panic. */
void interrupts_set_fault_hook(bool (*fn)(registers_t *regs));
#endif
