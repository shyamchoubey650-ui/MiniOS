/* PS/2 keyboard driver, interrupt-driven on IRQ1 (Phase 3; Phase 2 was polled).
 * The IRQ handler reads the scancode and pushes ASCII into a ring buffer. */
#include "include/keyboard.h"
#include "include/interrupts.h"
#include "include/ports.h"

#define KBD_DATA     0x60
#define KBD_BUF_SIZE 128
#define SC_RELEASE   0x80
#define SC_LSHIFT    0x2A
#define SC_RSHIFT    0x36
#define SC_CTRL      0x1D
#define SC_EXT_PREFIX 0xE0

static const char map_normal[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
};
static const char map_shift[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
};

static volatile int buf[KBD_BUF_SIZE];
static volatile uint32_t head, tail;
static bool shift, ctrl, extended;
static bool (*ctrl_c_hook)(void);

void keyboard_set_ctrl_c_hook(bool (*fn)(void)) { ctrl_c_hook = fn; }

/* what the keyboard sent last: lets `kbdinfo` show whether a key (PageUp, say) reaches the kernel and as which codes */
static volatile uint8_t sc_trace[8], sc_count_n;
static volatile int key_trace[8], key_count_n;
static void push(int c);
void keyboard_inject(int key) { push(key); }
void keyboard_trace(uint8_t sc[8], int *nsc, int keys[8], int *nkeys) {
    int n = sc_count_n, m = key_count_n;
    for (int i = 0; i < n; i++) sc[i] = sc_trace[8 - n + i];
    for (int i = 0; i < m; i++) keys[i] = key_trace[8 - m + i];
    *nsc = n; *nkeys = m;
}

static void push(int c) {
    for (int i = 0; i < 7; i++) key_trace[i] = key_trace[i + 1];
    key_trace[7] = c; if (key_count_n < 8) key_count_n++;
    uint32_t next = (head + 1) % KBD_BUF_SIZE;
    if (next == tail) return;           /* buffer full: drop key (documented) */
    buf[head] = c; head = next;
}

static void keyboard_irq(registers_t *regs) {
    (void)regs;
    uint8_t sc = inb(KBD_DATA);
    for (int i = 0; i < 7; i++) sc_trace[i] = sc_trace[i + 1];
    sc_trace[7] = sc; if (sc_count_n < 8) sc_count_n++;

    if (sc == SC_EXT_PREFIX) { extended = true; return; }   /* next byte is an extended key */

    bool release = sc & SC_RELEASE;
    sc &= (uint8_t)~SC_RELEASE;

    if (extended) {
        extended = false;
        if (release) return;
        switch (sc) {
            case 0x48: push(ctrl ? KEY_SCROLLUP : KEY_UP);   break;
            case 0x50: push(ctrl ? KEY_SCROLLDN : KEY_DOWN); break;
            case 0x4B: push(KEY_LEFT);   break;
            case 0x4D: push(KEY_RIGHT);  break;
            case 0x53: push(KEY_DELETE); break;
            case 0x47: push(KEY_HOME);   break;
            case 0x4F: push(KEY_END);    break;
            case 0x49: push(KEY_PGUP);   break;
            case 0x51: push(KEY_PGDN);   break;
            default: break;
        }
        return;
    }

    if (sc == SC_LSHIFT || sc == SC_RSHIFT) { shift = !release; return; }
    if (sc == SC_CTRL) { ctrl = !release; return; }
    if (release || sc >= 128) return;

    char c = shift ? map_shift[sc] : map_normal[sc];
    if (!c) return;
    if (ctrl && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);   /* Ctrl+letter */
    if (c == KEY_CTRL_C && ctrl_c_hook && ctrl_c_hook()) return;   /* consumed (e.g. aborts 'wait') */
    push(c);
}

void keyboard_init(void) {
    head = tail = 0;
    interrupt_register(IRQ_BASE + IRQ_KEYBOARD, keyboard_irq);
    while (inb(0x64) & 1) (void)inb(KBD_DATA);   /* flush stale bytes */
    irq_unmask(IRQ_KEYBOARD);
}

int keyboard_has_key(void) { return head != tail; }

int keyboard_getkey(void) {
    for (;;) {
        interrupts_disable();
        if (head != tail) {
            int c = buf[tail]; tail = (tail + 1) % KBD_BUF_SIZE;
            interrupts_enable();
            return c;
        }
        /* sti;hlt is atomic: no lost-wakeup window */
        __asm__ volatile ("sti; hlt");
    }
}

void keyboard_state(bool *sh, bool *ct, uint32_t *buffered, uint32_t *capacity) {
    if (sh) *sh = shift;
    if (ct) *ct = ctrl;
    if (buffered) *buffered = (head + KBD_BUF_SIZE - tail) % KBD_BUF_SIZE;
    if (capacity) *capacity = KBD_BUF_SIZE - 1;
}
