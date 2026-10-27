#include "include/mousepkt.h"

void mp_init(mp_t *m, bool wheel) { m->n = 0; m->wheel = wheel; m->packets = m->resyncs = m->dropped = 0; }

bool mp_feed(mp_t *m, uint8_t byte, mp_event_t *ev) {
    if (m->n == 0 && !(byte & 0x08)) { m->resyncs++; return false; }       /* not the start of a packet: skip it until one is found */
    m->b[m->n++] = byte;
    int need = m->wheel ? 4 : 3;
    if (m->n < need) return false;
    m->n = 0;
    uint8_t f = m->b[0];
    if (f & 0xC0) { m->dropped++; return false; }                           /* X or Y overflow: the movement is meaningless */
    ev->left = f & 1; ev->right = f & 2; ev->middle = f & 4;
    ev->dx = (int)m->b[1] - ((f & 0x10) ? 256 : 0);
    ev->dy = (int)m->b[2] - ((f & 0x20) ? 256 : 0);
    ev->dz = m->wheel ? (int)(int8_t)m->b[3] : 0;
    m->packets++;
    return true;
}

int mp_scroll_steps(int dz, int max) {
    if (max < 0) max = 0;
    if (dz < 0) return -dz > max ? max : -dz;           /* wheel up */
    if (dz > 0) return dz > max ? -max : -dz;           /* wheel down */
    return 0;
}
