/* MiniOS Phase 7 - boot/context.S
 * void arch_context_switch(uint32_t *old_esp, uint32_t new_esp)
 *
 * cdecl: the caller already preserves eax/ecx/edx, so only the callee-saved
 * registers (ebp, ebx, esi, edi) need saving. Stack frame left behind:
 *      [edi][esi][ebx][ebp][return address]   <- saved esp points at edi
 * arch_stack_init() fabricates exactly this frame for a brand-new task. */
.section .text
.global arch_context_switch
.type arch_context_switch, @function
arch_context_switch:
    mov  4(%esp), %eax        /* &old->esp   */
    mov  8(%esp), %edx        /* new esp     */
    push %ebp
    push %ebx
    push %esi
    push %edi
    mov  %esp, (%eax)         /* remember where this task stopped */
    mov  %edx, %esp           /* ---- now running on the other stack ---- */
    pop  %edi
    pop  %esi
    pop  %ebx
    pop  %ebp
    ret                       /* resume the other task (or enter a new one) */

.section .note.GNU-stack,"",@progbits
