/* PIC (8259) setup, handler registration, and the C-level dispatcher.
 *
 * Exceptions (0-31): CPU faults. Divide-by-zero etc. Unhandled ones print a
 *   register dump and halt (kernel panic). Breakpoint (3) is recoverable.
 * IRQs (32-47): hardware interrupts routed through the PIC.
 *   IRQ0 = PIT timer, IRQ1 = keyboard. */
#include "include/interrupts.h"
#include "include/logger.h"
#include "include/ports.h"
#include "include/screen.h"
#include "include/gdt.h"
#include "include/idt.h"

#define PIC1_CMD 0x20
#define PIC1_DAT 0x21
#define PIC2_CMD 0xA0
#define PIC2_DAT 0xA1
#define PIC_EOI  0x20
#define ICW1_INIT 0x11
#define ICW4_8086 0x01
#define NUM_VECTORS 256            /* handlers can be registered for every vector (0x80 = system calls) */
#define IRQ_COUNT   16

static interrupt_handler_t handlers[NUM_VECTORS];
static volatile uint32_t irq_counts[IRQ_COUNT];     /* how often each IRQ fired (irqinfo) */
uint32_t interrupt_irq_count(unsigned irq) { return irq < IRQ_COUNT ? irq_counts[irq] : 0; }
bool     interrupt_irq_masked(unsigned irq) {
    uint16_t port = irq < 8 ? PIC1_DAT : PIC2_DAT;
    return (inb(port) >> (irq & 7)) & 1;
}
static void (*exit_hook)(void);
static bool (*fault_hook)(registers_t *regs);

void interrupts_set_exit_hook(void (*fn)(void))              { exit_hook = fn; }
void interrupts_set_fault_hook(bool (*fn)(registers_t *regs)) { fault_hook = fn; }

static const char *names[32] = {
    "Divide by zero", "Debug", "Non-maskable interrupt", "Breakpoint",
    "Overflow", "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor overrun", "Invalid TSS", "Segment not present",
    "Stack-segment fault", "General protection fault", "Page fault", "Reserved",
    "x87 FPU error", "Alignment check", "Machine check", "SIMD FP exception",
    "Virtualization", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Security", "Reserved"
};

const char *exception_name(uint32_t v) { return v < 32 ? names[v] : "Unknown"; }

static void pic_remap(void) {
    /* Default PIC vectors (8-15) collide with CPU exceptions; move to 32-47. */
    outb(PIC1_CMD, ICW1_INIT); io_wait();
    outb(PIC2_CMD, ICW1_INIT); io_wait();
    outb(PIC1_DAT, IRQ_BASE);     io_wait();
    outb(PIC2_DAT, IRQ_BASE + 8); io_wait();
    outb(PIC1_DAT, 4); io_wait();   /* slave on IRQ2 */
    outb(PIC2_DAT, 2); io_wait();
    outb(PIC1_DAT, ICW4_8086); io_wait();
    outb(PIC2_DAT, ICW4_8086); io_wait();
    outb(PIC1_DAT, 0xFF);           /* mask everything until drivers unmask */
    outb(PIC2_DAT, 0xFF);
}

void irq_unmask(uint8_t irq) {
    uint16_t port = irq < 8 ? PIC1_DAT : PIC2_DAT;
    if (irq >= 8) { irq -= 8; outb(PIC1_DAT, inb(PIC1_DAT) & ~(1 << 2)); } /* cascade */
    outb(port, inb(port) & (uint8_t)~(1 << irq));
}

void irq_mask(uint8_t irq) {
    uint16_t port = irq < 8 ? PIC1_DAT : PIC2_DAT;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) | (uint8_t)(1 << irq));
}

void interrupt_register(uint32_t vector, interrupt_handler_t h) {
    if (vector < NUM_VECTORS) handlers[vector] = h;
}

void interrupts_enable(void)  { __asm__ volatile ("sti"); }
void interrupts_disable(void) { __asm__ volatile ("cli"); }

void interrupts_init(void) {
    gdt_init();
    pic_remap();
    idt_init();
}

static void panic_dump(registers_t *r) {
    interrupts_disable();
    klogf(LOG_ERROR, "cpu", "KERNEL PANIC: %s at eip=%x", exception_name(r->int_no), r->eip);
    screen_set_color(VGA_WHITE, VGA_RED);
    screen_puts("\n*** KERNEL PANIC: ");
    screen_puts(exception_name(r->int_no));
    screen_puts(" ***\n");
    screen_puts("vector="); screen_put_dec(r->int_no);
    screen_puts(" err=");   screen_put_hex(r->err_code);
    screen_puts("\nEIP="); screen_put_hex(r->eip);
    screen_puts(" CS=");   screen_put_hex(r->cs);
    screen_puts(" EFLAGS="); screen_put_hex(r->eflags);
    screen_puts("\nEAX="); screen_put_hex(r->eax);
    screen_puts(" EBX=");  screen_put_hex(r->ebx);
    screen_puts(" ECX=");  screen_put_hex(r->ecx);
    screen_puts(" EDX=");  screen_put_hex(r->edx);
    screen_puts("\nSystem halted.\n");
    for (;;) __asm__ volatile ("hlt");
}

/* Called from isr_common in boot/interrupts.S */
void interrupt_dispatch(registers_t *regs) {
    uint32_t v = regs->int_no;
    bool is_irq = v >= IRQ_BASE && v < IRQ_BASE + IRQ_COUNT;      /* only these come from the PIC */

    /* Acknowledge hardware IRQs FIRST: the exit hook below may switch to another
     * task, and this stack may not run again for a long time. Interrupts stay
     * disabled (interrupt gate), so no nested IRQ can sneak in. */
    if (is_irq) {
        irq_counts[v - IRQ_BASE]++;
        if (v >= IRQ_BASE + 8) outb(PIC2_CMD, PIC_EOI);
        outb(PIC1_CMD, PIC_EOI);
    }

    if (v < NUM_VECTORS && handlers[v]) handlers[v](regs);
    else if (v < 32) {                          /* unhandled exception */
        if (!(fault_hook && fault_hook(regs))) panic_dump(regs);
    }

    if (is_irq && exit_hook) exit_hook();
}
