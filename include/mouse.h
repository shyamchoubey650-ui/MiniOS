#ifndef MOUSE_H
#define MOUSE_H
/* PS/2 mouse (Phase 16). The only thing MiniOS does with it: the WHEEL scrolls the console. Each notch is injected into the keyboard
 * queue as Ctrl+Up / Ctrl+Down (KEY_SCROLLUP / KEY_SCROLLDN), so the shell and the login prompt scroll with no extra code.
 * QEMU only passes the mouse to the guest while its window has captured it: click inside the window (release with Ctrl+Alt+G), or
 * switch on View > Grab On Hover. Buttons and movement are read and counted but not used. */
#include "include/types.h"

typedef struct { bool present, wheel; uint32_t packets, notches, resyncs, dropped; uint8_t id; } mouse_info_t;
bool mouse_init(void);                      /* true if a mouse answered; returns quickly (bounded waits) if none is attached */
void mouse_info(mouse_info_t *out);
#endif
