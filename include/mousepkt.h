#ifndef MOUSEPKT_H
#define MOUSEPKT_H
/* PS/2 mouse packet decoder (Phase 16). Pure logic, no hardware: kernel/device/mouse.c feeds it the bytes from the mouse and
 * unit tests feed it made-up ones. A standard packet is 3 bytes; the wheel mode ("IntelliMouse", device ID 3) adds a 4th.
 *   byte 0: bit0 left, bit1 right, bit2 middle, bit3 ALWAYS 1 (used to stay in sync), bit4 X sign, bit5 Y sign, bit6 X overflow, bit7 Y overflow
 *   byte 1: X movement, byte 2: Y movement (up is positive), byte 3 (wheel mode): signed wheel movement, positive = wheel DOWN (toward the user). */
#include "include/types.h"

typedef struct { int dx, dy, dz; bool left, right, middle; } mp_event_t;
typedef struct { uint8_t b[4]; int n; bool wheel; uint32_t packets, resyncs, dropped; } mp_t;

void mp_init(mp_t *m, bool wheel);
bool mp_feed(mp_t *m, uint8_t byte, mp_event_t *ev);   /* true when this byte completed a good packet (ev is filled) */
/* How many scroll steps a wheel movement means: > 0 = scroll back (wheel up), < 0 = scroll forward (wheel down); at most `max` either way. */
int  mp_scroll_steps(int dz, int max);
#endif
