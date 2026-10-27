/* PS/2 mouse on the second port of the 8042 keyboard controller (IRQ12). i386 only; hardware, so no host test:
 * the packet decoding lives in lib/mousepkt.c and is tested there. */
#include "include/mouse.h"
#include "include/mousepkt.h"
#include "include/keyboard.h"
#include "include/interrupts.h"
#include "include/ports.h"

#define KBC_DATA   0x60
#define KBC_STATUS 0x64
#define KBC_CMD    0x64
#define IRQ_MOUSE  12
#define SCROLL_MAX_PER_PACKET 5

static mp_t decoder;
static mouse_info_t info;

/* All waits are bounded: a missing mouse (or a controller that does not answer) must never hang the boot. */
static bool wait_write(void) { for (int i = 0; i < 100000; i++) { if (!(inb(KBC_STATUS) & 2)) return true; io_wait(); } return false; }
static bool wait_read(void)  { for (int i = 0; i < 100000; i++) { if (inb(KBC_STATUS) & 1) return true; io_wait(); } return false; }
static int  read_data(void)  { return wait_read() ? inb(KBC_DATA) : -1; }
static bool kbc_command(uint8_t c) { if (!wait_write()) return false; outb(KBC_CMD, c); return true; }
static bool mouse_send(uint8_t b) {                              /* to the mouse: 0xD4 first, then the byte */
    if (!kbc_command(0xD4) || !wait_write()) return false;
    outb(KBC_DATA, b);
    return true;
}
static bool mouse_cmd(uint8_t b) {                               /* send and expect the 0xFA acknowledge */
    if (!mouse_send(b)) return false;
    return read_data() == 0xFA;
}

static void mouse_irq(registers_t *regs) {
    (void)regs;
    if (!(inb(KBC_STATUS) & 0x20)) return;                        /* the byte in the controller is not from the mouse */
    uint8_t byte = inb(KBC_DATA);
    mp_event_t ev;
    if (!mp_feed(&decoder, byte, &ev)) return;
    if (ev.dz != 0) {
        int steps = mp_scroll_steps(ev.dz, SCROLL_MAX_PER_PACKET);
        for (int i = 0; i < (steps < 0 ? -steps : steps); i++) { keyboard_inject(steps > 0 ? KEY_SCROLLUP : KEY_SCROLLDN); info.notches++; }
    }
}

bool mouse_init(void) {
    info.present = info.wheel = false;
    for (int i = 0; i < 16 && (inb(KBC_STATUS) & 1); i++) (void)inb(KBC_DATA);       /* flush stale bytes */
    if (!kbc_command(0xA8)) return false;                          /* enable the second (mouse) port */
    if (!kbc_command(0x20)) return false;                          /* read the controller configuration byte */
    int cfg = read_data(); if (cfg < 0) return false;
    cfg |= 0x02;                                                   /* enable IRQ12 */
    cfg &= ~0x20;                                                  /* enable the mouse clock */
    if (!kbc_command(0x60) || !wait_write()) return false;
    outb(KBC_DATA, (uint8_t)cfg);
    if (!mouse_cmd(0xF6)) return false;                            /* defaults: if nothing acknowledges, there is no mouse */
    /* the magic sample-rate sequence 200, 100, 80 switches an IntelliMouse into wheel mode; its device ID then reads 3 */
    static const uint8_t magic[3] = { 200, 100, 80 };
    for (int i = 0; i < 3; i++) { if (!mouse_cmd(0xF3) || !mouse_cmd(magic[i])) return false; }
    if (!mouse_cmd(0xF2)) return false;
    int id = read_data();
    info.id = id < 0 ? 0 : (uint8_t)id;
    info.wheel = id == 3;
    mp_init(&decoder, info.wheel);
    interrupt_register(IRQ_BASE + IRQ_MOUSE, mouse_irq);           /* the handler first, then let the mouse talk */
    if (!mouse_cmd(0xF4)) return false;                            /* start sending packets */
    irq_unmask(IRQ_MOUSE);
    info.present = true;
    return true;
}

void mouse_info(mouse_info_t *out) {
    *out = info;
    out->packets = decoder.packets; out->resyncs = decoder.resyncs; out->dropped = decoder.dropped;
}
