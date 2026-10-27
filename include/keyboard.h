#ifndef KEYBOARD_H
#define KEYBOARD_H
#include "include/types.h"
/* Special keys are returned as values above the ASCII range. */
#define KEY_UP     0x101
#define KEY_DOWN   0x102
#define KEY_LEFT   0x103
#define KEY_RIGHT  0x104
#define KEY_DELETE 0x105
#define KEY_HOME   0x106
#define KEY_END    0x107
#define KEY_PGUP   0x108
#define KEY_PGDN   0x109
#define KEY_SCROLLUP 0x10A      /* Ctrl+Up   : scroll back 3 lines (for keyboards without a PageUp key) */
#define KEY_SCROLLDN 0x10B      /* Ctrl+Down : scroll forward 3 lines */
#define KEY_CTRL_C 3
#define KEY_CTRL_L 12
void keyboard_init(void);
int  keyboard_has_key(void);
void keyboard_set_ctrl_c_hook(bool (*fn)(void)); /* IRQ context; return true to consume the key */
void keyboard_inject(int key);               /* puts a key into the queue as if it had been typed (the mouse wheel uses this) */
void keyboard_trace(uint8_t sc[8], int *nsc, int keys[8], int *nkeys);   /* the last raw scancode bytes and the last key codes, oldest first (kbdinfo shows them) */
void keyboard_state(bool *shift, bool *ctrl, uint32_t *buffered, uint32_t *capacity);   /* modifier keys and queued keys */
int  keyboard_getkey(void);      /* blocks (sti;hlt) until a key arrives */
#endif
