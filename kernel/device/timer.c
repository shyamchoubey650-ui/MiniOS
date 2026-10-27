/* Programmable Interval Timer (8253/8254) on IRQ0. REAL hardware.
 * Base oscillator is 1,193,182 Hz; we program a divisor to get TIMER_HZ. */
#include "include/timer.h"
#include "include/interrupts.h"
#include "include/ports.h"

#define PIT_CH0     0x40
#define PIT_CMD     0x43
#define PIT_BASE_HZ 1193182u
#define PIT_MODE    0x36       /* ch0, lobyte/hibyte, mode 3 square wave */

static volatile uint32_t ticks;
static uint32_t divisor_used, hz_used;
uint32_t timer_divisor(void) { return divisor_used; }
uint32_t timer_hz(void) { return hz_used; }
static void (*tick_hook)(void);

void timer_set_tick_hook(void (*fn)(void)) { tick_hook = fn; }
static void timer_irq(registers_t *regs) { (void)regs; ticks++; if (tick_hook) tick_hook(); }

void timer_init(uint32_t hz) {
    if (hz == 0) hz = TIMER_HZ;
    uint32_t div = PIT_BASE_HZ / hz;
    divisor_used = div; hz_used = hz;
    outb(PIT_CMD, PIT_MODE);
    outb(PIT_CH0, (uint8_t)(div & 0xFF));
    outb(PIT_CH0, (uint8_t)((div >> 8) & 0xFF));
    interrupt_register(IRQ_BASE + IRQ_TIMER, timer_irq);
    irq_unmask(IRQ_TIMER);
}

uint32_t timer_ticks(void) { return ticks; }
uint32_t timer_uptime_seconds(void) { return ticks / TIMER_HZ; }

void timer_sleep_ms(uint32_t ms) {
    uint32_t target = ticks + (ms * TIMER_HZ + 999) / 1000;
    while (ticks < target) __asm__ volatile ("hlt");   /* wake on next IRQ */
}
