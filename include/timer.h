#ifndef TIMER_H
#define TIMER_H
#include "include/types.h"
#define TIMER_HZ 100                 /* ticks per second */
void     timer_init(uint32_t hz);
uint32_t timer_ticks(void);
uint32_t timer_uptime_seconds(void);
void     timer_set_tick_hook(void (*fn)(void)); /* called from IRQ0 on every tick */
uint32_t timer_divisor(void);        /* PIT reload value actually programmed */
uint32_t timer_hz(void);
void     timer_sleep_ms(uint32_t ms); /* needs interrupts enabled */
#endif
