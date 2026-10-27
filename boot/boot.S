/* MiniOS Phase 3 - boot/boot.S
 * Multiboot header, kernel entry, stack setup. */
.set MB_MAGIC,    0x1BADB002
.set MB_FLAGS,    0x00000003          /* align modules + memory info */
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.section .multiboot
.align 4
    .long MB_MAGIC
    .long MB_FLAGS
    .long MB_CHECKSUM

.section .bss
.align 16
stack_bottom:
    .skip 16384
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    cli
    mov $stack_top, %esp
    push %ebx                 /* multiboot info pointer */
    push %eax                 /* multiboot magic        */
    call kernel_main
.hang:
    cli
    hlt
    jmp .hang

.section .note.GNU-stack,"",@progbits
