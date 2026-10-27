/* Unit tests for lib/mousepkt.c: the PS/2 mouse packet decoder (the hardware side is in kernel/device/mouse.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/mousepkt.h"

static int run, failed;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; printf("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static bool feed3(mp_t *m, uint8_t a, uint8_t b, uint8_t c, mp_event_t *ev) { return !mp_feed(m, a, ev) && !mp_feed(m, b, ev) && mp_feed(m, c, ev); }
static bool feed4(mp_t *m, uint8_t a, uint8_t b, uint8_t c, uint8_t d, mp_event_t *ev) { return !mp_feed(m, a, ev) && !mp_feed(m, b, ev) && !mp_feed(m, c, ev) && mp_feed(m, d, ev); }

static void test_plain(void) {
    puts("3-byte packets");
    mp_t m; mp_event_t e; mp_init(&m, false);
    CHECK(feed3(&m, 0x08, 5, 7, &e) && e.dx == 5 && e.dy == 7 && e.dz == 0 && !e.left && !e.right && !e.middle);
    CHECK(feed3(&m, 0x09, 0, 0, &e) && e.left && !e.right && !e.middle);
    CHECK(feed3(&m, 0x0A, 0, 0, &e) && e.right && !e.left);
    CHECK(feed3(&m, 0x0C, 0, 0, &e) && e.middle);
    CHECK(feed3(&m, 0x0F, 0, 0, &e) && e.left && e.right && e.middle);
    puts("negative movement uses the sign bits");
    CHECK(feed3(&m, 0x08 | 0x10, 0xFB, 0, &e) && e.dx == -5);                      /* X sign set, 0xFB = 251 -> -5 */
    CHECK(feed3(&m, 0x08 | 0x20, 0, 0xF0, &e) && e.dy == -16);
    CHECK(feed3(&m, 0x08 | 0x30, 0x80, 0x80, &e) && e.dx == -128 && e.dy == -128);
    CHECK(feed3(&m, 0x08, 0xFF, 0xFF, &e) && e.dx == 255 && e.dy == 255);          /* without the sign bits the byte is positive */
    CHECK(feed3(&m, 0x08, 0, 0, &e) && e.dx == 0 && e.dy == 0);
    CHECK(m.packets == 10 && m.resyncs == 0 && m.dropped == 0);
    puts("a plain mouse never reports a wheel");
    CHECK(feed3(&m, 0x08, 1, 1, &e) && e.dz == 0);
}

static void test_wheel(void) {
    puts("4-byte wheel packets");
    mp_t m; mp_event_t e; mp_init(&m, true);
    CHECK(feed4(&m, 0x08, 0, 0, 0x01, &e) && e.dz == 1);                           /* wheel down */
    CHECK(feed4(&m, 0x08, 0, 0, 0xFF, &e) && e.dz == -1);                          /* wheel up */
    CHECK(feed4(&m, 0x08, 0, 0, 0xFE, &e) && e.dz == -2);
    CHECK(feed4(&m, 0x08, 0, 0, 0x03, &e) && e.dz == 3);
    CHECK(feed4(&m, 0x08, 0, 0, 0x7F, &e) && e.dz == 127);
    CHECK(feed4(&m, 0x08, 0, 0, 0x80, &e) && e.dz == -128);
    CHECK(feed4(&m, 0x08, 0, 0, 0x00, &e) && e.dz == 0);
    CHECK(feed4(&m, 0x09, 3, 4, 0xFF, &e) && e.left && e.dx == 3 && e.dy == 4 && e.dz == -1);
    CHECK(m.packets == 8);
    puts("the 4th byte is part of the packet: it is never mistaken for the start of the next one");
    CHECK(feed4(&m, 0x08, 0, 0, 0x08, &e) && e.dz == 8 && m.resyncs == 0);          /* 0x08 has bit 3 set, like a first byte */
}

static void test_sync(void) {
    puts("resync: bytes that cannot start a packet are skipped");
    mp_t m; mp_event_t e; mp_init(&m, false);
    CHECK(!mp_feed(&m, 0x00, &e) && !mp_feed(&m, 0x77, &e) && !mp_feed(&m, 0xF0, &e) && m.resyncs == 3 && m.n == 0);
    CHECK(feed3(&m, 0x08, 2, 3, &e) && e.dx == 2 && e.dy == 3);
    puts("a packet split across calls");
    CHECK(!mp_feed(&m, 0x09, &e) && m.n == 1 && !mp_feed(&m, 10, &e) && m.n == 2 && mp_feed(&m, 20, &e) && m.n == 0 && e.dx == 10 && e.dy == 20 && e.left);
    puts("a lost byte: the packet after it still decodes once a start byte is seen");
    mp_init(&m, false);
    CHECK(!mp_feed(&m, 0x08, &e) && !mp_feed(&m, 5, &e));                           /* third byte never arrives ... */
    CHECK(mp_feed(&m, 0x08, &e) && e.dx == 5);                                       /* ... the next first byte is eaten as the third: one packet is garbled, then we are back in step */
    CHECK(feed3(&m, 0x08, 9, 9, &e) && e.dx == 9);
    puts("overflow packets are dropped and counted");
    mp_init(&m, false);
    CHECK(!mp_feed(&m, 0x48, &e) && !mp_feed(&m, 0, &e) && !mp_feed(&m, 0, &e) && m.dropped == 1 && m.packets == 0);       /* X overflow */
    CHECK(!mp_feed(&m, 0x88, &e) && !mp_feed(&m, 0, &e) && !mp_feed(&m, 0, &e) && m.dropped == 2);                        /* Y overflow */
    CHECK(!mp_feed(&m, 0xC8, &e) && !mp_feed(&m, 0, &e) && !mp_feed(&m, 0, &e) && m.dropped == 3);
    CHECK(feed3(&m, 0x08, 1, 1, &e) && m.packets == 1);
}

static void test_scroll_steps(void) {
    puts("scroll steps from the wheel movement");
    CHECK(mp_scroll_steps(0, 5) == 0);
    CHECK(mp_scroll_steps(-1, 5) == 1 && mp_scroll_steps(-3, 5) == 3 && mp_scroll_steps(-5, 5) == 5 && mp_scroll_steps(-6, 5) == 5 && mp_scroll_steps(-128, 5) == 5);     /* up = positive = scroll back */
    CHECK(mp_scroll_steps(1, 5) == -1 && mp_scroll_steps(4, 5) == -4 && mp_scroll_steps(5, 5) == -5 && mp_scroll_steps(9, 5) == -5 && mp_scroll_steps(127, 5) == -5);   /* down = negative = scroll forward */
    CHECK(mp_scroll_steps(-3, 0) == 0 && mp_scroll_steps(3, 0) == 0 && mp_scroll_steps(-3, -4) == 0 && mp_scroll_steps(3, 1) == -1);
}

static void test_fuzz(void) {
    puts("random bytes never break the decoder");
    srand(11);
    for (int wheel = 0; wheel < 2; wheel++) {
        mp_t m; mp_event_t e; mp_init(&m, wheel);
        uint32_t good = 0;
        for (int i = 0; i < 200000; i++) {
            uint8_t b = (uint8_t)rand();
            if (mp_feed(&m, b, &e)) { good++; if (e.dx < -256 || e.dx > 255 || e.dy < -256 || e.dy > 255 || e.dz < -128 || e.dz > 127 || (!wheel && e.dz != 0)) { CHECK(false); break; } }
            if (m.n < 0 || m.n >= (wheel ? 4 : 3)) { CHECK(false); break; }
        }
        CHECK(good == m.packets && good > 1000 && m.resyncs > 0 && m.dropped > 0);
    }
    puts("a long stream of valid packets decodes exactly");
    mp_t m; mp_event_t e; mp_init(&m, true); int count = 0;
    for (int i = 0; i < 50000; i++) { uint8_t dz = (uint8_t)(i % 5 == 0 ? 0xFF : 0); mp_feed(&m, 0x08, &e); mp_feed(&m, (uint8_t)(i & 0x7F), &e); mp_feed(&m, 1, &e); if (mp_feed(&m, dz, &e)) { count++; if (e.dx != (i & 0x7F) || e.dz != (i % 5 == 0 ? -1 : 0)) { CHECK(false); break; } } }
    CHECK(count == 50000 && m.resyncs == 0);
}

int main(void) {
    test_plain(); test_wheel(); test_sync(); test_scroll_steps(); test_fuzz();
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
