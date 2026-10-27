#include "include/scrollback.h"

void sb_init(sb_t *s) { s->head = 0; s->count = 0; }
void sb_push(sb_t *s, const uint16_t *row) {
    for (int c = 0; c < SB_COLS; c++) s->line[s->head][c] = row[c];
    s->head = (s->head + 1) % SB_LINES;
    if (s->count < SB_LINES) s->count++;
}
int sb_count(const sb_t *s) { return s->count; }
const uint16_t *sb_get(const sb_t *s, int i) {
    if (i < 0 || i >= s->count) return 0;
    int idx = (s->head - s->count + i + 2 * SB_LINES) % SB_LINES;
    return s->line[idx];
}
int sb_clamp(const sb_t *s, int off) { return off < 0 ? 0 : off > s->count ? s->count : off; }
const uint16_t *sb_view_row(const sb_t *s, const uint16_t *live, int rows, int off, int r) {
    (void)rows;
    int v = s->count - sb_clamp(s, off) + r;                       /* index in [saved lines ... live rows] */
    if (v < s->count) return sb_get(s, v);
    return live + (v - s->count) * SB_COLS;
}
static bool blank_row(const uint16_t *row) { for (int c = 0; c < SB_COLS; c++) if ((row[c] & 0xFF) != ' ' && (row[c] & 0xFF) != 0) return false; return true; }
void sb_push_screen(sb_t *s, const uint16_t *screen, int rows) {
    int last = -1;
    for (int r = 0; r < rows; r++) if (!blank_row(screen + r * SB_COLS)) last = r;
    for (int r = 0; r <= last; r++) sb_push(s, screen + r * SB_COLS);
}
