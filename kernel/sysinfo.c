/* Kernel information and power control (see include/kinfo.h). Every query fills a fixed-size structure;
 * the system-call layer copies it to the caller. */
#include "include/kinfo.h"
#include "include/syscall.h"
#include "include/memory.h"
#include "include/timer.h"
#include "include/string.h"
#include "include/ramdisk.h"
#ifndef MINIOS_HOST_TEST
#include "include/ports.h"
#include "include/interrupts.h"
#include "include/screen.h"
#include "include/keyboard.h"
#include "include/mouse.h"
#endif

static multiboot_info_t boot_copy; static bool have_boot;
static uintptr_t k_start, k_data, k_end;

void kinfo_set_boot(const multiboot_info_t *mbi) { if (mbi) { boot_copy = *mbi; have_boot = true; } }
void kinfo_set_kernel(uintptr_t s, uintptr_t d, uintptr_t e) { k_start = s; k_data = d; k_end = e; }

/* ---------- pure helpers ---------- */
uint8_t kinfo_bcd(uint8_t v) { return (uint8_t)((v & 0x0F) + ((v >> 4) * 10)); }

uint8_t kinfo_weekday(uint16_t y, uint8_t m, uint8_t d) {      /* Sakamoto's method */
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (uint8_t)((y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7);
}

uint32_t kinfo_size(int what) {
    switch (what) {
        case KI_CPU: return sizeof(ki_cpu_t);       case KI_CPUID: return sizeof(ki_cpuid_t);
        case KI_TIMER: return sizeof(ki_timer_t);   case KI_IRQ: return sizeof(ki_irq_t);
        case KI_IDT_INFO: case KI_GDT_INFO: return sizeof(ki_table_t);
        case KI_IDT_ENTRY: case KI_GDT_ENTRY: return sizeof(ki_desc_t);
        case KI_KERNEL: return sizeof(ki_kernel_t); case KI_BOOT: return sizeof(ki_boot_t);
        case KI_MMAP: return sizeof(ki_mmap_t);     case KI_VGA: return sizeof(ki_vga_t);
        case KI_KBD: return sizeof(ki_kbd_t);       case KI_RTC: return sizeof(ki_rtc_t);
        case KI_DEVICE: return sizeof(ki_device_t); default: return 0;
    }
}

/* ---------- CPUID (real instruction on the host too) ---------- */
static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static void put32(char *dst, uint32_t v) { memcpy(dst, &v, 4); }

static void fill_cpu(ki_cpu_t *o) {
    memset(o, 0, sizeof *o);
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    o->max_leaf = a; put32(o->vendor, b); put32(o->vendor + 4, d); put32(o->vendor + 8, c);
    if (a >= 1) {
        cpuid(1, 0, &a, &b, &c, &d);
        o->stepping = a & 15; o->model = (a >> 4) & 15; o->family = (a >> 8) & 15;
        if (o->family == 15) o->family += (a >> 20) & 255;
        if (o->family == 6 || o->family >= 15) o->model += ((a >> 16) & 15) << 4;
        o->feat_edx = d; o->feat_ecx = c;
    }
    cpuid(0x80000000u, 0, &a, &b, &c, &d);
    if (a >= 0x80000004u) for (uint32_t i = 0; i < 3; i++) {
        cpuid(0x80000002u + i, 0, &a, &b, &c, &d);
        put32(o->brand + 16 * i, a); put32(o->brand + 16 * i + 4, b); put32(o->brand + 16 * i + 8, c); put32(o->brand + 16 * i + 12, d);
    }
    /* trim leading spaces of the brand string */
    size_t lead = 0; while (o->brand[lead] == ' ') lead++;
    if (lead) { size_t n = strlen(o->brand + lead) + 1; for (size_t i = 0; i < n; i++) o->brand[i] = o->brand[lead + i]; }
}

/* ---------- RTC ---------- */
#ifndef MINIOS_HOST_TEST
static uint8_t cmos(uint8_t reg) { outb(0x70, reg); io_wait(); return inb(0x71); }
static void read_rtc(ki_rtc_t *o) {
    uint8_t s, mi, h, d, mo, y, regb;
    for (int guard = 0; guard < 100000 && (cmos(0x0A) & 0x80); guard++) { }       /* wait out an update in progress */
    do { s = cmos(0); mi = cmos(2); h = cmos(4); d = cmos(7); mo = cmos(8); y = cmos(9); regb = cmos(0x0B);
         /* read twice until both reads agree, so a rollover cannot give a torn value */
    } while (s != cmos(0) || mi != cmos(2) || h != cmos(4));
    bool pm = h & 0x80; h &= 0x7F;
    if (!(regb & 4)) { s = kinfo_bcd(s); mi = kinfo_bcd(mi); h = kinfo_bcd(h); d = kinfo_bcd(d); mo = kinfo_bcd(mo); y = kinfo_bcd(y); }
    if (!(regb & 2)) { if (pm && h < 12) h = (uint8_t)(h + 12); else if (!pm && h == 12) h = 0; }   /* 12-hour clock */
    o->second = s; o->minute = mi; o->hour = h; o->day = d; o->month = mo; o->year = (uint16_t)(2000 + y);
}
#else
static void read_rtc(ki_rtc_t *o) { o->year = 2026; o->month = 10; o->day = 12; o->hour = 9; o->minute = 30; o->second = 15; }
#endif

/* ---------- descriptor tables ---------- */
#ifndef MINIOS_HOST_TEST
struct dt_ptr { uint16_t limit; uint32_t base; } __attribute__((packed));
static void fill_table(ki_table_t *o, bool idt) {
    struct dt_ptr p;
    if (idt) __asm__ volatile ("sidt %0" : "=m"(p)); else __asm__ volatile ("sgdt %0" : "=m"(p));
    o->base = p.base; o->limit = p.limit;
}
static int fill_idt_entry(uint32_t i, ki_desc_t *o) {
    ki_table_t t; fill_table(&t, true);
    if (i >= (t.limit + 1) / 8) return SYS_EINVAL;
    const uint8_t *g = (const uint8_t *)(uintptr_t)(t.base + i * 8);
    o->addr = g[0] | (g[1] << 8) | (g[6] << 16) | ((uint32_t)g[7] << 24); o->extra = g[2] | (g[3] << 8);
    o->flags = g[5]; o->present = g[5] & 0x80; o->dpl = (g[5] >> 5) & 3; o->type = g[5] & 15;
    return 0;
}
static int fill_gdt_entry(uint32_t i, ki_desc_t *o) {
    ki_table_t t; fill_table(&t, false);
    if (i >= (t.limit + 1) / 8) return SYS_EINVAL;
    const uint8_t *g = (const uint8_t *)(uintptr_t)(t.base + i * 8);
    o->addr = g[2] | (g[3] << 8) | (g[4] << 16) | ((uint32_t)g[7] << 24);
    uint32_t lim = g[0] | (g[1] << 8) | ((g[6] & 15) << 16);
    if (g[6] & 0x80) lim = (lim << 12) | 0xFFF;                  /* 4 KiB granularity */
    o->extra = lim; o->flags = g[6] >> 4; o->present = g[5] & 0x80; o->dpl = (g[5] >> 5) & 3; o->type = g[5] & 15;
    return 0;
}
#else
static void fill_table(ki_table_t *o, bool idt) { o->base = idt ? 0x2000 : 0x1000; o->limit = idt ? 2047 : 23; }
static int fill_idt_entry(uint32_t i, ki_desc_t *o) { if (i >= 256) return SYS_EINVAL; o->addr = 0x100000 + i * 16; o->extra = 8; o->flags = 0x8E; o->present = true; o->dpl = 0; o->type = 14; return 0; }
static int fill_gdt_entry(uint32_t i, ki_desc_t *o) { if (i >= 3) return SYS_EINVAL; o->addr = 0; o->extra = i ? 0xFFFFFFFFu : 0; o->flags = 0xC; o->present = i != 0; o->dpl = 0; o->type = i == 1 ? 10 : 2; return 0; }
#endif

/* ---------- devices ---------- */
static const struct { const char *name, *kind; int irq; const char *desc; } devices[] = {
    { "kbd0", "input",   1,  "PS/2 keyboard, scancode set 1, interrupt-driven ring buffer" },
    { "vga0", "display", -1, "VGA text mode 80x25, memory-mapped at 0xB8000" },
    { "pit0", "timer",   0,  "8253/8254 PIT channel 0, drives the scheduler" },
    { "rtc0", "clock",   -1, "CMOS real-time clock (read only, polled)" },
    { "ram0", "block",   -1, "RAM-disk (kernel heap), 512-byte sectors" },
};
#define N_DEVICES (sizeof devices / sizeof devices[0])

/* ---------- the query ---------- */
int kinfo_get(int what, uint32_t index, void *buf, uint32_t size) {
    uint32_t need = kinfo_size(what);
    if (need == 0) return SYS_EINVAL;
    if (size < need) return SYS_EINVAL;
    switch (what) {
        case KI_CPU:   fill_cpu(buf); return 0;
        case KI_CPUID: { ki_cpuid_t *q = buf; cpuid(q->leaf, q->subleaf, &q->eax, &q->ebx, &q->ecx, &q->edx); return 0; }
        case KI_TIMER: {
            ki_timer_t *t = buf; memset(t, 0, sizeof *t);
            t->ticks = timer_ticks(); t->base_hz = 1193182u;
#ifndef MINIOS_HOST_TEST
            t->hz = timer_hz(); t->divisor = timer_divisor();
#else
            t->hz = TIMER_HZ; t->divisor = t->base_hz / TIMER_HZ;
#endif
            return 0;
        }
        case KI_IRQ: {
            ki_irq_t *q = buf; memset(q, 0, sizeof *q);
            if (index >= 16) return SYS_EINVAL;
            static const char *const names[16] = { "timer", "keyboard", "cascade", "serial2", "serial1", "parallel2", "floppy", "parallel1",
                                                   "rtc", "free", "free", "free", "mouse", "fpu", "ide-primary", "ide-secondary" };
            const char *n = names[index]; size_t i = 0; while (n[i] && i < 15) { q->name[i] = n[i]; i++; }
#ifndef MINIOS_HOST_TEST
            q->count = interrupt_irq_count(index); q->masked = interrupt_irq_masked(index);
#else
            q->count = index == 0 ? timer_ticks() : 0; q->masked = index > 1;
#endif
            return 0;
        }
        case KI_IDT_INFO: fill_table(buf, true); return 0;
        case KI_GDT_INFO: fill_table(buf, false); return 0;
        case KI_IDT_ENTRY: memset(buf, 0, sizeof(ki_desc_t)); return fill_idt_entry(index, buf);
        case KI_GDT_ENTRY: memset(buf, 0, sizeof(ki_desc_t)); return fill_gdt_entry(index, buf);
        case KI_KERNEL: {
            ki_kernel_t *k = buf; memory_summary_t m; memory_summary(&m);
            k->start = (uint32_t)k_start; k->data_start = (uint32_t)k_data; k->end = (uint32_t)k_end; k->size = m.kernel_bytes;
            k->heap_base = m.heap_base; k->heap_size = m.heap_size; k->boot_stack = 16384u;
            return 0;
        }
        case KI_BOOT: {
            ki_boot_t *b = buf; memset(b, 0, sizeof *b);
            if (!have_boot) return SYS_ENOENT;
            b->flags = boot_copy.flags; b->mem_lower = boot_copy.mem_lower; b->mem_upper = boot_copy.mem_upper;
            b->mmap_entries = (uint32_t)memory_regions_seen();
            return 0;
        }
        case KI_MMAP: {
            ki_mmap_t *e = buf; const mem_region_t *r = memory_region(index);
            if (!r) return SYS_ENOENT;
            e->addr = r->base; e->len = r->length; e->type = r->type;
            return 0;
        }
        case KI_VGA: {
            ki_vga_t *v = buf; memset(v, 0, sizeof *v);
            v->cols = 80; v->rows = 25; v->base = 0xB8000;
#ifndef MINIOS_HOST_TEST
            int r, c; uint8_t at; screen_state(&r, &c, &at); v->cursor_row = (uint32_t)r; v->cursor_col = (uint32_t)c; v->color = at;
#else
            v->color = 0x07;
#endif
            return 0;
        }
        case KI_KBD: {
            ki_kbd_t *k = buf; memset(k, 0, sizeof *k);
#ifndef MINIOS_HOST_TEST
            keyboard_state(&k->shift, &k->ctrl, &k->buffered, &k->capacity);
            { int nsc = 0, nk = 0, keys[8]; uint8_t sc[8]; keyboard_trace(sc, &nsc, keys, &nk);
              { mouse_info_t mi; mouse_info(&mi); k->mouse = mi.present; k->mouse_wheel = mi.wheel; k->mouse_id = mi.id; k->mouse_packets = mi.packets; k->mouse_notches = mi.notches; k->mouse_resyncs = mi.resyncs; k->mouse_dropped = mi.dropped; }
              k->nsc = (uint8_t)nsc; k->nkeys = (uint8_t)nk; for (int i = 0; i < nsc; i++) k->scancodes[i] = sc[i]; for (int i = 0; i < nk; i++) k->keys[i] = keys[i]; }
#else
            k->capacity = 127;
#endif
            return 0;
        }
        case KI_RTC: {
            ki_rtc_t *r = buf; memset(r, 0, sizeof *r); read_rtc(r);
            if (r->month < 1 || r->month > 12 || r->day < 1 || r->day > 31) return SYS_EINVAL;   /* clock returned garbage */
            r->weekday = kinfo_weekday(r->year, r->month, r->day);
            return 0;
        }
        case KI_DEVICE: {
            ki_device_t *d = buf; memset(d, 0, sizeof *d);
            if (index >= N_DEVICES) return SYS_ENOENT;
            size_t i;
            for (i = 0; devices[index].name[i] && i < 11; i++) d->name[i] = devices[index].name[i];
            for (i = 0; devices[index].kind[i] && i < 9; i++) d->kind[i] = devices[index].kind[i];
            for (i = 0; devices[index].desc[i] && i < 55; i++) d->desc[i] = devices[index].desc[i];
            d->irq = devices[index].irq;
            return 0;
        }
    }
    return SYS_EINVAL;
}

/* ---------- PC speaker ---------- */
static bool beep_on; static uint32_t beep_hz;
bool kinfo_beep_state(uint32_t *hz) { if (hz) *hz = beep_hz; return beep_on; }
int kinfo_beep(bool on, uint32_t hz) {
    if (on && (hz < 37 || hz > 20000)) return SYS_EINVAL;               /* below 37 Hz the 16-bit PIT divisor overflows */
#ifndef MINIOS_HOST_TEST
    if (on) {
        uint32_t div = 1193182u / hz;
        outb(0x43, 0xB6); outb(0x42, (uint8_t)(div & 0xFF)); outb(0x42, (uint8_t)(div >> 8));     /* PIT channel 2, square wave */
        outb(0x61, (uint8_t)(inb(0x61) | 3));                                                     /* gate it and connect the speaker */
    } else outb(0x61, (uint8_t)(inb(0x61) & ~3));
#endif
    beep_on = on; beep_hz = on ? hz : 0;
    return 0;
}

/* ---------- power ---------- */
int kinfo_power(int op) {
    if (op != POWER_REBOOT && op != POWER_SHUTDOWN && op != POWER_HALT) return SYS_EINVAL;
#ifdef MINIOS_HOST_TEST
    return 0;
#else
    __asm__ volatile ("cli");
    if (op == POWER_REBOOT) {
        for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) { }          /* wait for the keyboard controller input buffer */
        outb(0x64, 0xFE);                                                 /* pulse the CPU reset line */
        struct dt_ptr zero = { 0, 0 };                                    /* fallback: load an empty IDT and fault -> triple fault */
        __asm__ volatile ("lidt %0; int3" : : "m"(zero));
    } else if (op == POWER_SHUTDOWN) {
        outw(0x604, 0x2000);                                              /* QEMU (newer) ACPI shutdown */
        outw(0xB004, 0x2000);                                             /* QEMU (older) / Bochs */
        outw(0x4004, 0x3400);                                             /* VirtualBox */
        screen_puts("\nIt is now safe to turn off the computer.\n");      /* real hardware without ACPI support here */
    } else screen_puts("\nSystem halted.\n");
    for (;;) __asm__ volatile ("hlt");
#endif
}
