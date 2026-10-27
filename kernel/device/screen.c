/* VGA text-mode driver (80x25, memory-mapped at 0xB8000). REAL hardware access. */
#include "include/screen.h"
#include "include/ports.h"
#include "include/irqflags.h"
#include "include/capture.h"
#include "include/scrollback.h"

#define VGA_MEM    ((volatile uint16_t *)0xB8000)
#define VGA_COLS   80
#define VGA_ROWS   25
#define CRTC_INDEX 0x3D4
#define CRTC_DATA  0x3D5

static int row, col;
static uint8_t color = (VGA_BLACK << 4) | VGA_LIGHT_GREY;

static uint16_t cell(char c) { return (uint16_t)((color << 8) | (uint8_t)c); }

static void update_cursor(void) {
    uint16_t pos = (uint16_t)(row * VGA_COLS + col);
    outb(CRTC_INDEX, 14); outb(CRTC_DATA, (uint8_t)(pos >> 8));
    outb(CRTC_INDEX, 15); outb(CRTC_DATA, (uint8_t)(pos & 0xFF));
}

/* ---- scrollback (Phase 16): the ring and the window logic are in lib/scrollback.c; this file owns the VGA memory ---- */
static sb_t sb;
static uint16_t live_snap[VGA_ROWS * VGA_COLS];          /* the live screen while an older part is shown */
static int view_off;                                     /* 0 = live; otherwise lines above the live screen */
static uint32_t scroll_events;

static void cursor_visible(bool on) {
    outb(CRTC_INDEX, 0x0A);
    outb(CRTC_DATA, on ? 14 : 0x20);                     /* bit 5 of the cursor-start register switches the cursor off */
}
static void copy_row_in(uint16_t *dst, int r) { for (int c = 0; c < VGA_COLS; c++) dst[c] = VGA_MEM[r * VGA_COLS + c]; }
static bool row_blank(int r) { for (int c = 0; c < VGA_COLS; c++) { uint16_t ch = VGA_MEM[r * VGA_COLS + c] & 0xFF; if (ch != ' ' && ch != 0) return false; } return true; }
static void remember_screen(void) {                      /* the screen is about to be cleared: keep what was on it */
    int last = -1; uint16_t tmp[VGA_COLS];
    for (int r = 0; r < VGA_ROWS; r++) if (!row_blank(r)) last = r;
    for (int r = 0; r <= last; r++) { copy_row_in(tmp, r); sb_push(&sb, tmp); }
}
static void render_view(void) {
    static const char note[] = " scrollback: PgUp/PgDn, any other key = back to the prompt ";
    for (int r = 0; r < VGA_ROWS; r++) {
        const uint16_t *src = sb_view_row(&sb, live_snap, VGA_ROWS, view_off, r);
        for (int c = 0; c < VGA_COLS; c++) VGA_MEM[r * VGA_COLS + c] = src[c];
    }
    int n = (int)sizeof note - 1, at = VGA_COLS - n;
    for (int i = 0; i < n; i++) VGA_MEM[at + i] = (uint16_t)(((VGA_BLUE << 4 | VGA_YELLOW) << 8) | (uint8_t)note[i]);
}
static void leave_view(void) {                           /* back to the live screen */
    if (!view_off) return;
    for (int i = 0; i < VGA_ROWS * VGA_COLS; i++) VGA_MEM[i] = live_snap[i];
    view_off = 0;
    cursor_visible(true);
    update_cursor();
}

static void scroll(void) {
    uint16_t top[VGA_COLS];
    copy_row_in(top, 0);
    sb_push(&sb, top);                                   /* the line that leaves the top is kept */
    scroll_events++;
    for (int r = 1; r < VGA_ROWS; r++)
        for (int c = 0; c < VGA_COLS; c++)
            VGA_MEM[(r - 1) * VGA_COLS + c] = VGA_MEM[r * VGA_COLS + c];
    for (int c = 0; c < VGA_COLS; c++)
        VGA_MEM[(VGA_ROWS - 1) * VGA_COLS + c] = cell(' ');
    row = VGA_ROWS - 1;
}

static uint8_t default_color = (VGA_BLACK << 4) | VGA_LIGHT_GREY;     /* what 'light grey on black' means; changed by the color command */
/* Light grey on black always means "the default text colours": every command ends its coloured output with that pair,
 * so a colour chosen with the color command survives. */
void screen_set_color(uint8_t fg, uint8_t bg) {
    if (fg == VGA_LIGHT_GREY && bg == VGA_BLACK) { color = default_color; return; }
    color = (uint8_t)((bg << 4) | (fg & 0x0F));
}
void screen_set_default_color(uint8_t fg, uint8_t bg) { default_color = (uint8_t)((bg << 4) | (fg & 0x0F)); color = default_color; }

static void clear_unlocked(void) {
    leave_view();
    remember_screen();
    for (int i = 0; i < VGA_ROWS * VGA_COLS; i++) VGA_MEM[i] = cell(' ');
    row = col = 0;
    update_cursor();
}

void screen_clear(void) { uint32_t f = irq_save(); clear_unlocked(); irq_restore(f); }
void screen_init(void) { screen_clear(); sb_init(&sb); }        /* the boot loader's text, cleared away, is not history */

static void putc_unlocked(char c) {
    if (c == '\n') { col = 0; row++; }
    else if (c == '\r') { col = 0; }
    else if (c == '\t') { col = (col + 8) & ~7; }
    else {
        VGA_MEM[row * VGA_COLS + col] = cell(c);
        col++;
    }
    if (col >= VGA_COLS) { col = 0; row++; }
    if (row >= VGA_ROWS) scroll();
    update_cursor();
}

static void backspace_unlocked(void) {
    if (col > 0) col--;
    else if (row > 0) { row--; col = VGA_COLS - 1; }
    VGA_MEM[row * VGA_COLS + col] = cell(' ');
    update_cursor();
}

/* Each character update is atomic: a timer interrupt can switch tasks between
 * characters, but never in the middle of moving the cursor. */
void screen_putc(char c)    { uint32_t f = irq_save(); if (!capture_write(c)) { leave_view(); putc_unlocked(c); } irq_restore(f); }   /* redirected output goes to a buffer */
void screen_backspace(void) { uint32_t f = irq_save(); leave_view(); backspace_unlocked(); irq_restore(f); }

void screen_puts(const char *s) { while (*s) screen_putc(*s++); }

void screen_put_dec(uint32_t v) {
    char buf[11]; int i = 0;
    if (v == 0) { screen_putc('0'); return; }
    while (v) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) screen_putc(buf[--i]);
}

void screen_put_hex(uint32_t v) {
    static const char d[] = "0123456789ABCDEF";
    screen_puts("0x");
    for (int s = 28; s >= 0; s -= 4) screen_putc(d[(v >> s) & 0xF]);
}

void screen_state(int *r, int *c, uint8_t *attr) { if (r) *r = row; if (c) *c = col; if (attr) *attr = color; }

/* ---- cursor control and scrollback (Phase 16) ---- */
void screen_set_cursor(int r, int c) {
    uint32_t f = irq_save();
    leave_view();
    row = r < 0 ? 0 : r >= VGA_ROWS ? VGA_ROWS - 1 : r;
    col = c < 0 ? 0 : c >= VGA_COLS ? VGA_COLS - 1 : c;
    update_cursor();
    irq_restore(f);
}
uint32_t screen_scroll_count(void) { return scroll_events; }
void screen_scrollback(int lines) {
    uint32_t f = irq_save();
    int target = sb_clamp(&sb, view_off + lines);
    if (target == view_off) { irq_restore(f); return; }
    if (view_off == 0) {                                  /* leaving the live screen: remember it */
        for (int i = 0; i < VGA_ROWS * VGA_COLS; i++) live_snap[i] = VGA_MEM[i];
        cursor_visible(false);
    }
    view_off = target;
    if (view_off == 0) { view_off = 1; leave_view(); }    /* back to the bottom: restore the live screen */
    else render_view();
    irq_restore(f);
}
void screen_scrollback_reset(void) { uint32_t f = irq_save(); leave_view(); irq_restore(f); }
bool screen_scrollback_active(void) { return view_off != 0; }
int  screen_scrollback_lines(void) { return sb_count(&sb); }
