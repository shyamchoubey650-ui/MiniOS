/* System-information commands (Phase 11): date time cpuinfo cpuid ticks timerinfo irqinfo idtinfo gdtinfo kernelinfo
 * bootinfo vgainfo kbdinfo devices reboot shutdown halt arch osname about.
 * All data comes from sys_kinfo() (kernel/sysinfo.c): real CPUID, real descriptor tables, the real CMOS clock, real counters. */
#include "include/builtins.h"
#include "include/lineedit.h"
#include "include/keyboard.h"
#include "include/cmdutil.h"
#include "include/screen.h"
#include "include/syscall.h"
#include "include/kinfo.h"
#include "include/timer.h"
#include "include/version.h"
#include "include/numparse.h"
#include "include/string.h"

static int ki(const char *cmd, int what, uint32_t idx, void *out, uint32_t size) {
    int rc = sys_kinfo(what, idx, out, size);
    if (rc != 0) cu_error(cmd, sys_strerror(rc));
    return rc;
}

static void label(const char *s) { cu_pad(s, 16); screen_puts(": "); }
static void hex(uint32_t v) { screen_put_hex(v); }
static void two(unsigned v) { screen_putc((char)('0' + v / 10)); screen_putc((char)('0' + v % 10)); }

/* ---------- clock ---------- */
static const char *const day_names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const month_names[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };

static int cmd_date(int argc, const char **argv) {
    (void)argv; ki_rtc_t r;
    if (argc != 1) return cu_usage("date");
    if (ki("date", KI_RTC, 0, &r, sizeof r)) return 1;
    screen_puts(day_names[r.weekday % 7]); screen_puts(", "); screen_put_dec(r.day); screen_putc(' '); screen_puts(month_names[(r.month - 1) % 12]); screen_putc(' '); screen_put_dec(r.year); screen_putc('\n');
    return 0;
}
static int cmd_time(int argc, const char **argv) {
    (void)argv; ki_rtc_t r;
    if (argc != 1) return cu_usage("time");
    if (ki("time", KI_RTC, 0, &r, sizeof r)) return 1;
    two(r.hour); screen_putc(':'); two(r.minute); screen_putc(':'); two(r.second); screen_puts("  (CMOS clock; QEMU takes it from the host, usually UTC)\n");
    return 0;
}

/* ---------- CPU ---------- */
static int cmd_cpuinfo(int argc, const char **argv) {
    (void)argv; ki_cpu_t c;
    if (argc != 1) return cu_usage("cpuinfo");
    if (ki("cpuinfo", KI_CPU, 0, &c, sizeof c)) return 1;
    label("Vendor"); screen_puts(c.vendor); screen_putc('\n');
    label("Model name"); screen_puts(c.brand[0] ? c.brand : "(not reported)"); screen_putc('\n');
    label("Family/Model"); screen_put_dec(c.family); screen_puts(" / "); screen_put_dec(c.model); screen_puts("  stepping "); screen_put_dec(c.stepping); screen_putc('\n');
    label("Highest leaf"); hex(c.max_leaf); screen_putc('\n');
    static const struct { uint8_t bit; const char *name; } edx[] = { {0,"fpu"},{4,"tsc"},{5,"msr"},{6,"pae"},{9,"apic"},{15,"cmov"},{23,"mmx"},{24,"fxsr"},{25,"sse"},{26,"sse2"} };
    static const struct { uint8_t bit; const char *name; } ecx[] = { {0,"sse3"},{9,"ssse3"},{19,"sse4.1"},{20,"sse4.2"},{28,"avx"},{31,"hypervisor"} };
    label("Features");
    for (size_t i = 0; i < sizeof edx / sizeof edx[0]; i++) if (c.feat_edx & (1u << edx[i].bit)) { screen_puts(edx[i].name); screen_putc(' '); }
    for (size_t i = 0; i < sizeof ecx / sizeof ecx[0]; i++) if (c.feat_ecx & (1u << ecx[i].bit)) { screen_puts(ecx[i].name); screen_putc(' '); }
    screen_putc('\n');
    screen_puts("(MiniOS uses none of the SSE/AVX state: it runs integer code only)\n");
    return 0;
}

static int cmd_cpuid(int argc, const char **argv) {
    ki_cpuid_t q = { 0, 0, 0, 0, 0, 0 };
    if (argc < 2 || argc > 3) return cu_usage("cpuid <leaf> [subleaf]     e.g. cpuid 0   or   cpuid 0x80000002");
    if (!parse_u32(argv[1], &q.leaf) || (argc == 3 && !parse_u32(argv[2], &q.subleaf))) return cu_error("cpuid", "leaf and subleaf are numbers (decimal or 0x...).");
    if (ki("cpuid", KI_CPUID, 0, &q, sizeof q)) return 1;
    screen_puts("EAX="); hex(q.eax); screen_puts("  EBX="); hex(q.ebx); screen_puts("\nECX="); hex(q.ecx); screen_puts("  EDX="); hex(q.edx); screen_putc('\n');
    if (q.leaf == 0 || q.leaf == 0x80000002u || q.leaf == 0x80000003u || q.leaf == 0x80000004u) {   /* these leaves hold text */
        const uint32_t w[4] = { q.leaf == 0 ? q.ebx : q.eax, q.leaf == 0 ? q.edx : q.ebx, q.leaf == 0 ? q.ecx : q.ecx, q.leaf == 0 ? 0 : q.edx };
        screen_puts("as text: \"");
        for (int i = 0; i < 4; i++) for (int k = 0; k < 4; k++) { char ch = (char)(w[i] >> (8 * k)); if (ch >= 32 && ch < 127) screen_putc(ch); }
        screen_puts("\"\n");
    }
    return 0;
}

/* ---------- timer and interrupts ---------- */
static int cmd_ticks(int argc, const char **argv) {
    (void)argv; ki_timer_t t;
    if (argc != 1) return cu_usage("ticks");
    if (ki("ticks", KI_TIMER, 0, &t, sizeof t)) return 1;
    screen_put_dec(t.ticks); screen_puts(" ticks since boot (" ); screen_put_dec(t.hz ? t.ticks / t.hz : 0); screen_puts(" s at "); screen_put_dec(t.hz); screen_puts(" Hz)\n");
    return 0;
}
static int cmd_timerinfo(int argc, const char **argv) {
    (void)argv; ki_timer_t t;
    if (argc != 1) return cu_usage("timerinfo");
    if (ki("timerinfo", KI_TIMER, 0, &t, sizeof t)) return 1;
    label("Device"); screen_puts("8253/8254 PIT, channel 0, IRQ0, square-wave mode\n");
    label("Input clock"); screen_put_dec(t.base_hz); screen_puts(" Hz\n");
    label("Reload value"); screen_put_dec(t.divisor); screen_putc('\n');
    label("Tick rate"); screen_put_dec(t.hz); screen_puts(" Hz  (one tick = "); screen_put_dec(t.hz ? 1000 / t.hz : 0); screen_puts(" ms)\n");
    label("Ticks so far"); screen_put_dec(t.ticks); screen_putc('\n');
    return 0;
}
static int cmd_irqinfo(int argc, const char **argv) {
    (void)argv; ki_irq_t q;
    if (argc != 1) return cu_usage("irqinfo");
    screen_puts("IRQ  NAME            STATE      COUNT\n");
    for (uint32_t i = 0; i < 16; i++) {
        if (ki("irqinfo", KI_IRQ, i, &q, sizeof q)) return 1;
        cu_num_pad(i, 3); screen_puts("  "); cu_pad(q.name, 14); screen_puts("  "); cu_pad(q.masked ? "masked" : "ENABLED", 9); screen_putc(' '); cu_num_pad(q.count, 9); screen_putc('\n');
    }
    screen_puts("(MiniOS enables only the timer and the keyboard; the rest stay masked)\n");
    return 0;
}

static int table_dump(const char *cmd, bool idt, int argc, const char **argv) {
    ki_table_t t; uint32_t start = 0, count = idt ? 16 : 8;
    if (argc > 3) return cu_usage(idt ? "idtinfo [first [count]]" : "gdtinfo [first [count]]");
    if ((argc >= 2 && !cu_parse_count(argv[1], &start, 255)) || (argc == 3 && !cu_parse_count(argv[2], &count, 64))) return cu_error(cmd, "first and count must be small numbers.");
    if (ki(cmd, idt ? KI_IDT_INFO : KI_GDT_INFO, 0, &t, sizeof t)) return 1;
    uint32_t total = (t.limit + 1) / 8;
    screen_puts(idt ? "IDTR" : "GDTR"); screen_puts(": base "); hex(t.base); screen_puts(", limit "); screen_put_dec(t.limit); screen_puts(" -> "); screen_put_dec(total); screen_puts(" entries\n");
    screen_puts(idt ? "VEC  HANDLER     SEL   TYPE  DPL  P\n" : "NO   BASE        LIMIT       ACCESS  DPL  P\n");
    for (uint32_t i = start; i < start + count && i < total; i++) {
        ki_desc_t d;
        if (ki(cmd, idt ? KI_IDT_ENTRY : KI_GDT_ENTRY, i, &d, sizeof d)) return 1;
        cu_num_pad(i, 3); screen_puts("  "); hex(d.addr); screen_puts("  ");
        if (idt) { screen_put_hex(d.extra & 0xFFFF); }
        else { hex(d.extra); }
        screen_puts("  ");
        if (idt) { screen_put_dec(d.type); screen_puts(d.type == 14 ? " (int32)" : d.type == 15 ? " (trap32)" : ""); } else { screen_put_hex(d.type | (d.dpl << 5) | (d.present ? 0x80 : 0)); }
        screen_puts("  "); screen_put_dec(d.dpl); screen_puts("  "); screen_puts(d.present ? "yes" : "no"); screen_putc('\n');
    }
    return 0;
}
static int cmd_idtinfo(int argc, const char **argv) { return table_dump("idtinfo", true, argc, argv); }
static int cmd_gdtinfo(int argc, const char **argv) { return table_dump("gdtinfo", false, argc, argv); }

/* ---------- kernel, boot, devices ---------- */
static int cmd_kernelinfo(int argc, const char **argv) {
    (void)argv; ki_kernel_t k;
    if (argc != 1) return cu_usage("kernelinfo");
    if (ki("kernelinfo", KI_KERNEL, 0, &k, sizeof k)) return 1;
    label("Image start"); hex(k.start); screen_putc('\n');
    label("Code/rodata end"); hex(k.data_start); screen_puts("  (write-protected from system calls)\n");
    label("Image end"); hex(k.end); screen_putc('\n');
    label("Image size"); cu_put_size(k.size); screen_putc('\n');
    label("Heap"); hex(k.heap_base); screen_puts(", "); cu_put_size(k.heap_size); screen_putc('\n');
    label("Task stack size"); cu_put_size(k.boot_stack); screen_putc('\n');
    return 0;
}
static int cmd_bootinfo(int argc, const char **argv) {
    (void)argv; ki_boot_t b;
    if (argc != 1) return cu_usage("bootinfo");
    if (ki("bootinfo", KI_BOOT, 0, &b, sizeof b)) return 1;
    label("Loader"); screen_puts("GRUB (Multiboot 1)\n");
    label("Flags"); hex(b.flags); screen_putc('\n');
    label("Lower memory"); screen_put_dec(b.mem_lower); screen_puts(" KiB\n");
    label("Upper memory"); screen_put_dec(b.mem_upper); screen_puts(" KiB\n");
    screen_puts("Memory map ("); screen_put_dec(b.mmap_entries); screen_puts(" entries):\n");
    for (uint32_t i = 0; i < b.mmap_entries && i < 16; i++) {
        ki_mmap_t m;
        if (sys_kinfo(KI_MMAP, i, &m, sizeof m) != 0) break;
        screen_puts("  "); hex((uint32_t)m.addr); screen_puts(" + "); cu_put_size((uint32_t)m.len); screen_puts("  "); screen_puts(m.type == 1 ? "available" : "reserved"); screen_putc('\n');
    }
    return 0;
}
static int cmd_vgainfo(int argc, const char **argv) {
    (void)argv; ki_vga_t v;
    if (argc != 1) return cu_usage("vgainfo");
    if (ki("vgainfo", KI_VGA, 0, &v, sizeof v)) return 1;
    label("Mode"); screen_put_dec(v.cols); screen_putc('x'); screen_put_dec(v.rows); screen_puts(" text, 16 colours\n");
    label("Video memory"); hex(v.base); screen_puts("  (2 bytes per cell: character + colour)\n");
    label("Cursor"); screen_puts("row "); screen_put_dec(v.cursor_row); screen_puts(", column "); screen_put_dec(v.cursor_col); screen_putc('\n');
    label("Colour byte"); screen_put_hex(v.color); screen_puts("  (foreground "); screen_put_dec(v.color & 15); screen_puts(", background "); screen_put_dec(v.color >> 4); screen_puts(")\n");
    return 0;
}
static int cmd_kbdinfo(int argc, const char **argv) {
    (void)argv; ki_kbd_t k;
    if (argc != 1) return cu_usage("kbdinfo");
    if (ki("kbdinfo", KI_KBD, 0, &k, sizeof k)) return 1;
    label("Controller"); screen_puts("PS/2, IRQ1, scancode set 1, US layout\n");
    label("Shift held"); screen_puts(k.shift ? "yes\n" : "no\n");
    label("Ctrl held"); screen_puts(k.ctrl ? "yes\n" : "no\n");
    label("Queued keys"); screen_put_dec(k.buffered); screen_puts(" of "); screen_put_dec(k.capacity); screen_putc('\n');
    label("Last scancodes");
    if (k.nsc == 0) screen_puts("none yet (press a key, then run kbdinfo again)");
    for (int i = 0; i < k.nsc; i++) { static const char hx[] = "0123456789ABCDEF"; screen_putc(hx[k.scancodes[i] >> 4]); screen_putc(hx[k.scancodes[i] & 15]); screen_putc(' '); }
    screen_putc('\n');
    label("Last keys");
    if (k.nkeys == 0) screen_puts("none yet");
    for (int i = 0; i < k.nkeys; i++) { char nm[24]; key_name(k.keys[i], nm, sizeof nm); screen_puts(nm); screen_putc(' '); }
    screen_putc('\n');
    label("Mouse");
    if (!k.mouse) screen_puts("none found (or not captured by the window)\n");
    else { screen_puts("PS/2, device ID "); screen_put_dec(k.mouse_id); screen_puts(k.mouse_wheel ? ", wheel: yes" : ", wheel: no"); screen_puts(", "); screen_put_dec(k.mouse_packets); screen_puts(" packets, "); screen_put_dec(k.mouse_notches); screen_puts(" wheel notches"); if (k.mouse_resyncs || k.mouse_dropped) { screen_puts(" ("); screen_put_dec(k.mouse_resyncs); screen_puts(" resyncs, "); screen_put_dec(k.mouse_dropped); screen_puts(" dropped)"); } screen_putc('\n'); }
    screen_puts("(PageUp should show up as scancodes E0 49 and key PageUp; Ctrl+Up and the mouse wheel scroll too)\n");
    return 0;
}
static int cmd_devices(int argc, const char **argv) {
    (void)argv; ki_device_t d;
    if (argc != 1) return cu_usage("devices");
    screen_puts("NAME   KIND     IRQ  DESCRIPTION\n");
    for (uint32_t i = 0; sys_kinfo(KI_DEVICE, i, &d, sizeof d) == 0; i++) {
        cu_pad(d.name, 6); screen_putc(' '); cu_pad(d.kind, 8); screen_putc(' ');
        if (d.irq >= 0) cu_num_pad((uint32_t)d.irq, 3); else screen_puts("  -");
        screen_puts("  "); screen_puts(d.desc); screen_putc('\n');
    }
    screen_puts("(a fixed list of the drivers MiniOS has; there is no plug-and-play or generic ioctl layer)\n");
    return 0;
}

/* ---------- power ---------- */
static int power(const char *cmd, int op, const char *msg) {
    cu_color(VGA_YELLOW); screen_puts(msg); cu_color(VGA_LIGHT_GREY); screen_putc('\n');
    int rc = sys_power(op);
    if (rc == SYS_EACCES) return cu_error(cmd, "only root may do this (use 'sudo' or 'su').");
    if (rc != 0) return cu_error(cmd, sys_strerror(rc));
    return 0;                                                     /* real hardware never gets here; the host test build does */
}
static int cmd_reboot(int argc, const char **argv)   { (void)argv; if (argc != 1) return cu_usage("reboot"); return power("reboot", POWER_REBOOT, "Rebooting..."); }
static int cmd_shutdown(int argc, const char **argv) { (void)argv; if (argc != 1) return cu_usage("shutdown"); return power("shutdown", POWER_SHUTDOWN, "Shutting down MiniOS..."); }
static int cmd_halt(int argc, const char **argv)     { (void)argv; if (argc != 1) return cu_usage("halt"); return power("halt", POWER_HALT, "Halting the CPU (reset the machine to start again)..."); }

/* ---------- identity ---------- */
static int cmd_arch(int argc, const char **argv) { (void)argv; if (argc != 1) return cu_usage("arch"); screen_puts("i386\n"); return 0; }
static int cmd_osname(int argc, const char **argv) { (void)argv; if (argc != 1) return cu_usage("osname"); screen_puts("MiniOS\n"); return 0; }
static int cmd_about(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("about");
    screen_puts("MiniOS " MINIOS_VERSION " (" MINIOS_PHASE ") - an educational operating system\n");
    screen_puts("Written in C and x86 assembly, booted by GRUB, runs in QEMU.\n");
    screen_puts("Real: interrupts, timer, keyboard, VGA, scheduler, heap, MiniFS, system calls, users and permissions.\n");
    screen_puts("Simulated or missing: networking, paging/virtual memory, ring 3, real disk drivers (see 'cmdinfo <name>').\n");
    return 0;
}

/* ---------- beep and clock (Phase 12) ---------- */
static int cmd_beep(int argc, const char **argv) {
    uint32_t hz = 880, ms = 300;
    if (argc > 3 || (argc >= 2 && (!cu_parse_count(argv[1], &hz, 20000) || hz < 37)) || (argc == 3 && (!cu_parse_count(argv[2], &ms, 2000) || ms < 10)))
        return cu_usage("beep [frequency 37-20000 Hz] [milliseconds 10-2000]      (default 880 Hz, 300 ms)");
    int rc = sys_beep(true, hz);
    if (rc != 0) return cu_error("beep", sys_strerror(rc));
    screen_puts("beep: "); screen_put_dec(hz); screen_puts(" Hz for "); screen_put_dec(ms); screen_puts(" ms (the PC speaker; QEMU is silent unless started with an audio backend for it)\n");
    cu_sleep_ms(ms);
    sys_beep(false, 0);                                           /* always switched off again */
    return 0;
}
static bool clock_stop_requested(void) {
    bool hit = false;
    while (keyboard_has_key()) { int k = keyboard_getkey(); if (k == 3 || k == 'q' || k == 'Q') hit = true; }
    return hit;
}
static int cmd_clock(int argc, const char **argv) {
    uint32_t limit = 0; ki_rtc_t r;
    if (argc == 3 && !strcmp(argv[1], "-n")) { if (!cu_parse_count(argv[2], &limit, 86400) || limit == 0) return cu_usage("clock [-n seconds]"); }
    else if (argc != 1) return cu_usage("clock [-n seconds]      (live clock; Ctrl+C or q stops it)");
    screen_puts("CMOS clock - Ctrl+C or q to stop\n");
    for (uint32_t shown = 0; ; shown++) {
        if (ki("clock", KI_RTC, 0, &r, sizeof r)) return 1;
        screen_putc('\r'); two(r.hour); screen_putc(':'); two(r.minute); screen_putc(':'); two(r.second); screen_puts("   ");
        screen_put_dec(r.day); screen_putc(' '); screen_puts(month_names[(r.month - 1) % 12]); screen_putc(' '); screen_put_dec(r.year);
        if ((limit && shown + 1 >= limit) || clock_stop_requested()) break;
        cu_sleep_ms(1000);
        if (clock_stop_requested()) break;
    }
    screen_putc('\n');
    return 0;
}


static const struct { const char *name; cmd_handler_t fn; } bindings[] = {
    { "date", cmd_date }, { "time", cmd_time }, { "cpuinfo", cmd_cpuinfo }, { "cpuid", cmd_cpuid }, { "ticks", cmd_ticks },
    { "timerinfo", cmd_timerinfo }, { "irqinfo", cmd_irqinfo }, { "idtinfo", cmd_idtinfo }, { "gdtinfo", cmd_gdtinfo },
    { "kernelinfo", cmd_kernelinfo }, { "bootinfo", cmd_bootinfo }, { "vgainfo", cmd_vgainfo }, { "kbdinfo", cmd_kbdinfo },
    { "devices", cmd_devices }, { "reboot", cmd_reboot }, { "shutdown", cmd_shutdown }, { "halt", cmd_halt },
    { "arch", cmd_arch }, { "osname", cmd_osname }, { "about", cmd_about }, { "beep", cmd_beep }, { "clock", cmd_clock },
};

int sysinfo_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
