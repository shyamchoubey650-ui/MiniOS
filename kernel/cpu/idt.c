/* Interrupt Descriptor Table: 256 gates; vectors 0-47 are wired to asm stubs. */
#include "include/idt.h"
#include "include/gdt.h"
#include "include/string.h"

#define IDT_ENTRIES   256
#define IDT_USED      48
#define GATE_INT32    0x8E   /* present, ring0, 32-bit interrupt gate */
#define GATE_SYSCALL  0xEE   /* present, DPL 3 (callable from user mode later), 32-bit interrupt gate */

extern void isr128(void);    /* boot/interrupts.S */

struct idt_entry {
    uint16_t base_low, selector;
    uint8_t  zero, flags;
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr { uint16_t limit; uint32_t base; } __attribute__((packed));

extern uint32_t isr_stub_table[];
static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   ip;

static void set_gate(int v, uint32_t handler, uint8_t flags) {
    idt[v].base_low  = handler & 0xFFFF;
    idt[v].base_high = (handler >> 16) & 0xFFFF;
    idt[v].selector  = GDT_KERNEL_CODE;
    idt[v].zero      = 0;
    idt[v].flags     = flags;
}

void idt_init(void) {
    memset(idt, 0, sizeof(idt));
    for (int i = 0; i < IDT_USED; i++) set_gate(i, isr_stub_table[i], GATE_INT32);
    set_gate(0x80, (uint32_t)(uintptr_t)isr128, GATE_SYSCALL);        /* system calls */
    ip.limit = sizeof(idt) - 1;
    ip.base  = (uint32_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(ip));
}
