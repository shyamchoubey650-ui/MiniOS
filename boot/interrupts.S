/* MiniOS Phase 3 - boot/interrupts.S
 * Low-level interrupt entry stubs for CPU exceptions (0-31)
 * and hardware IRQs (remapped to vectors 32-47).
 *
 * Every stub pushes a uniform frame, then jumps to isr_common which
 * calls the C dispatcher: void interrupt_dispatch(registers_t *regs). */

.macro ISR_NOERR num
.global isr\num
isr\num:
    push $0                   /* dummy error code */
    push $\num                /* vector number    */
    jmp isr_common
.endm

.macro ISR_ERR num
.global isr\num
isr\num:
    push $\num                /* CPU already pushed the error code */
    jmp isr_common
.endm

.macro IRQ num, vec
.global irq\num
irq\num:
    push $0
    push $\vec
    jmp isr_common
.endm

.section .text

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

/* System-call entry (int 0x80): same frame as everything else; the C handler reads eax/ebx/ecx/edx/esi
 * from the saved registers and stores the result back into the saved eax. */
.global isr128
isr128:
    push $0
    push $128
    jmp isr_common

isr_common:
    pusha                     /* eax,ecx,edx,ebx,esp,ebp,esi,edi */
    push %ds
    push %es
    push %fs
    push %gs
    mov $0x10, %ax            /* kernel data selector */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    cld
    push %esp                 /* argument: registers_t* */
    call interrupt_dispatch
    add $4, %esp
    pop %gs
    pop %fs
    pop %es
    pop %ds
    popa
    add $8, %esp              /* drop vector + error code */
    iret

/* Table of stub addresses, used by idt_init(). */
.section .data
.global isr_stub_table
isr_stub_table:
    .long isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7
    .long isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15
    .long isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .long isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    .long irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7
    .long irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15

.section .note.GNU-stack,"",@progbits
