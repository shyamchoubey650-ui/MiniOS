/* Host-side stand-in for boot/context.S: instead of swapping stacks it records the switch
 * and, if a test armed `stub_jump`, longjmps back to the test (meaning "this call blocked
 * and another task now owns the CPU"). The scheduler has already updated `current`. */
#include <setjmp.h>
#include "include/arch_context.h"

jmp_buf *stub_jump;
int      stub_switches;

void arch_context_switch(uint32_t *old_esp, uint32_t new_esp) {
    (void)new_esp;
    *old_esp = 0xE5E50000u + (uint32_t)stub_switches;
    stub_switches++;
    if (stub_jump) longjmp(*stub_jump, 1);
}
uint32_t arch_stack_init(uint8_t *top, void (*entry)(void)) { (void)entry; return (uint32_t)((uintptr_t)top & 0xFFFFFFF0u) - 24; }
void arch_idle(void) {}
