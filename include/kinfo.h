#ifndef KINFO_H
#define KINFO_H
/* Read-only kernel information for the system-information commands (cpuinfo, irqinfo, idtinfo, date, ...),
 * plus power control. Reached through SYS_KINFO / SYS_POWER; the structures are copied to the caller.
 *
 * REAL:      CPUID, the IDT/GDT as the CPU has them loaded (sidt/sgdt), PIT and IRQ counters, the CMOS real-time clock,
 *            the Multiboot data, VGA and keyboard state, reboot/shutdown through hardware ports.
 * HOST TEST: hardware reads (ports, sidt/sgdt, counters) return fixed values so the plumbing can be unit-tested. */
#include "include/types.h"
#include "include/multiboot.h"

enum { KI_CPU = 1, KI_CPUID, KI_TIMER, KI_IRQ, KI_IDT_INFO, KI_IDT_ENTRY, KI_GDT_INFO, KI_GDT_ENTRY,
       KI_KERNEL, KI_BOOT, KI_MMAP, KI_VGA, KI_KBD, KI_RTC, KI_DEVICE, KI_COUNT };

typedef struct { char vendor[13]; char brand[49]; uint32_t max_leaf, family, model, stepping, feat_edx, feat_ecx; } ki_cpu_t;
typedef struct { uint32_t leaf, subleaf, eax, ebx, ecx, edx; } ki_cpuid_t;          /* leaf/subleaf are INPUT */
typedef struct { uint32_t hz, ticks, divisor, base_hz; } ki_timer_t;
typedef struct { uint32_t count; bool masked; char name[16]; } ki_irq_t;            /* index = IRQ 0..15 */
typedef struct { uint32_t base, limit; } ki_table_t;                                /* IDT / GDT register */
typedef struct { uint32_t addr, extra; uint8_t type, dpl, flags; bool present; } ki_desc_t;   /* IDT: addr=handler, extra=selector; GDT: addr=base, extra=limit */
typedef struct { uint32_t start, data_start, end, size, heap_base, heap_size, boot_stack; } ki_kernel_t;
typedef struct { uint32_t flags, mem_lower, mem_upper, mmap_entries; } ki_boot_t;
typedef struct { uint64_t addr, len; uint32_t type; } ki_mmap_t;                    /* index = entry */
typedef struct { uint32_t cols, rows, base, cursor_row, cursor_col; uint8_t color; } ki_vga_t;
typedef struct { bool shift, ctrl; uint32_t buffered, capacity; uint8_t nsc, nkeys; uint8_t scancodes[8]; int32_t keys[8];
                bool mouse, mouse_wheel; uint8_t mouse_id; uint32_t mouse_packets, mouse_notches, mouse_resyncs, mouse_dropped; } ki_kbd_t;   /* the last raw bytes and key codes, oldest first */
typedef struct { uint16_t year; uint8_t month, day, hour, minute, second, weekday; } ki_rtc_t;   /* weekday 0 = Sunday */
typedef struct { char name[12]; char kind[10]; int32_t irq; char desc[56]; } ki_device_t;        /* index = device */

#define KI_MAX_STRUCT 128

/* kernel side */
void kinfo_set_boot(const multiboot_info_t *mbi);
void kinfo_set_kernel(uintptr_t start, uintptr_t data_start, uintptr_t end);
int  kinfo_get(int what, uint32_t index, void *buf, uint32_t size);      /* 0 or SYS_E*; buf also carries the input for KI_CPUID */
uint32_t kinfo_size(int what);                                            /* bytes the caller must provide; 0 = unknown selector */

/* power control */
#define POWER_REBOOT   1
#define POWER_SHUTDOWN 2
#define POWER_HALT     3
#define POWER_BEEP_ON  4      /* any user: PC speaker on at the frequency in the 2nd argument (37-20000 Hz)    */
#define POWER_BEEP_OFF 5      /* any user: PC speaker off                                                        */
int  kinfo_beep(bool on, uint32_t hz);                                    /* SYS_EINVAL for a frequency outside 37-20000 Hz */
bool kinfo_beep_state(uint32_t *hz);                                      /* is the speaker on (host tests read this)       */
int  kinfo_power(int op);                                                 /* does not return on success (host test: returns 0) */

/* pure helpers (tested on the host) */
uint8_t kinfo_weekday(uint16_t year, uint8_t month, uint8_t day);        /* 0 = Sunday */
uint8_t kinfo_bcd(uint8_t v);
#endif
