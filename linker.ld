ENTRY(_start)
SECTIONS
{
    . = 1M;
    kernel_start = .;
    .text   : ALIGN(4K) { *(.multiboot) *(.text*) }
    .rodata : ALIGN(4K) { *(.rodata*) }
    . = ALIGN(4K);
    kernel_data_start = .;           /* code + read-only data end here; below is write-protected for syscalls */
    .data   : ALIGN(4K) { *(.data*) }
    .bss    : ALIGN(4K) { *(COMMON) *(.bss*) }
    kernel_end = .;
}
