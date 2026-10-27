#ifndef SCROLLBACK_H
#define SCROLLBACK_H
/* Scrollback (Phase 16): the lines that scrolled off the top of the 80x25 screen, kept in a ring of 200 lines so PageUp can show them again.
 * Pure logic (no VGA access): kernel/device/screen.c owns the memory-mapped screen and uses this to decide what to draw. */
#include "include/types.h"

#define SB_COLS  80
#define SB_LINES 200
typedef struct { uint16_t line[SB_LINES][SB_COLS]; int head, count; } sb_t;     /* head = where the next line goes; count <= SB_LINES */

void            sb_init(sb_t *s);
void            sb_push(sb_t *s, const uint16_t *row);                              /* SB_COLS cells; the oldest line is dropped when full */
int             sb_count(const sb_t *s);
const uint16_t *sb_get(const sb_t *s, int i);                                       /* 0 = oldest; NULL if out of range */
int             sb_clamp(const sb_t *s, int off);                                   /* a view offset (lines above the live screen) limited to 0..count */
/* Which row of cells to draw on screen row r while the view is `off` lines above the live screen (live = rows*SB_COLS cells). */
const uint16_t *sb_view_row(const sb_t *s, const uint16_t *live, int rows, int off, int r);
/* Pushes the used part of a screen (rows 0..last row that has text) when it is cleared, so the cleared text can still be scrolled back to. */
void            sb_push_screen(sb_t *s, const uint16_t *screen, int rows);
#endif
