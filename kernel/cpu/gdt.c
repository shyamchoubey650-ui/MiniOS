/* Global Descriptor Table: flat 4 GiB model (null, kernel code, kernel data).
 * REAL: loaded into the CPU with lgdt. */
#include "include/gdt.h"

struct gdt_entry {
    uint16_t limit_low, base_low;
    uint8_t  base_mid, access, granularity, base_high;
} __attribute__((packed));

struct gdt_ptr { uint16_t limit; uint32_t base; } __attribute__((packed));

#define GDT_ENTRIES 3
static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gp;

static void set_gate(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[i].base_low    = base & 0xFFFF;
    gdt[i].base_mid    = (base >> 16) & 0xFF;
    gdt[i].base_high   = (base >> 24) & 0xFF;
    gdt[i].limit_low   = limit & 0xFFFF;
    gdt[i].granularity = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt[i].access      = access;
}

void gdt_init(void) {
    set_gate(0, 0, 0, 0, 0);                        /* null descriptor */
    set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);         /* ring0 code      */
    set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);         /* ring0 data      */
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (uint32_t)&gdt;
    __asm__ volatile (
        "lgdt %0\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n mov %%ax, %%es\n mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n mov %%ax, %%ss\n"
        "ljmp $0x08, $1f\n"
        "1:\n"
        : : "m"(gp) : "eax", "memory");
}
