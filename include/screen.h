#ifndef SCREEN_H
#define SCREEN_H
#include "include/types.h"
enum vga_color { VGA_BLACK=0, VGA_BLUE, VGA_GREEN, VGA_CYAN, VGA_RED, VGA_MAGENTA,
                 VGA_BROWN, VGA_LIGHT_GREY, VGA_DARK_GREY, VGA_LIGHT_BLUE,
                 VGA_LIGHT_GREEN, VGA_LIGHT_CYAN, VGA_LIGHT_RED, VGA_LIGHT_MAGENTA,
                 VGA_YELLOW, VGA_WHITE };
void screen_init(void);
void screen_clear(void);
void screen_set_color(uint8_t fg, uint8_t bg);
void screen_set_default_color(uint8_t fg, uint8_t bg);   /* the colours used whenever a command restores "light grey on black" */
void screen_putc(char c);
void screen_puts(const char *s);
void screen_put_dec(uint32_t v);
void screen_put_hex(uint32_t v);
void screen_backspace(void);
void screen_state(int *row, int *col, uint8_t *attr);
void screen_set_cursor(int row, int col);                /* moves the cursor (clamped to the 80x25 screen); used by the line editor */
uint32_t screen_scroll_count(void);                      /* how many times the screen has scrolled since boot */
/* Scrollback (Phase 16): the last 200 lines that scrolled off the top. Any output, clear or cursor move returns to the live screen. */
void screen_scrollback(int lines);                       /* > 0 = look further back, < 0 = come forward; reaching 0 returns to the live screen */
void screen_scrollback_reset(void);
bool screen_scrollback_active(void);
int  screen_scrollback_lines(void);                      /* lines currently kept */   /* cursor position and current colour attribute */
#endif
