#ifndef IRQFLAGS_H
#define IRQFLAGS_H
#include "include/types.h"
/* Save/restore the CPU interrupt flag. Used to make short kernel sections atomic
 * on this single-CPU kernel. Host-side tests compile these as no-ops. */
#ifdef MINIOS_HOST_TEST
static inline uint32_t irq_save(void)          { return 0; }
static inline void     irq_restore(uint32_t f) { (void)f; }
static inline void     irq_enable(void)        { }
static inline void     irq_set(uint32_t f)     { (void)f; }
#else
static inline uint32_t irq_save(void) {
    uint32_t flags;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
static inline void irq_restore(uint32_t flags) { if (flags & 0x200u) __asm__ volatile ("sti" : : : "memory"); }
static inline void irq_enable(void)            { __asm__ volatile ("sti" : : : "memory"); }
/* Makes the interrupt flag match a value saved earlier by irq_save() (enables OR disables). */
static inline void irq_set(uint32_t flags)     { if (flags & 0x200u) __asm__ volatile ("sti" : : : "memory"); else __asm__ volatile ("cli" : : : "memory"); }
#endif
#endif
