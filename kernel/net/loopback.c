#include "include/loopback.h"
#include "include/irqflags.h"
#include "include/string.h"

typedef struct { uint32_t port, head, count, sent; char msg[LO_QUEUE_DEPTH][LO_MSG_MAX + 1]; } queue_t;
static queue_t q[LO_MAX_PORTS];
static uint32_t total_sent;

static queue_t *find(uint32_t port, bool create) {
    for (size_t i = 0; i < LO_MAX_PORTS; i++) if (q[i].port == port) return &q[i];
    if (!create) return NULL;
    for (size_t i = 0; i < LO_MAX_PORTS; i++) if (q[i].port == 0) { q[i].port = port; q[i].head = q[i].count = q[i].sent = 0; return &q[i]; }
    return NULL;
}
static void copy_text(char *d, size_t cap, const char *s) {
    size_t i = 0;
    while (s[i] && i + 1 < cap) { unsigned char c = (unsigned char)s[i]; d[i] = (c < 32 || c > 126) ? '?' : (char)c; i++; }   /* no control characters can be smuggled to the receiver's screen */
    d[i] = '\0';
}

int lo_send(uint32_t port, const char *text) {
    if (port < 1 || port > 65535) return LO_ERR_PORT;
    if (!text) text = "";
    if (strlen(text) > LO_MSG_MAX) return LO_ERR_LONG;
    uint32_t f = irq_save();
    queue_t *p = find(port, true);
    if (!p || p->count >= LO_QUEUE_DEPTH) { irq_restore(f); return LO_ERR_FULL; }
    copy_text(p->msg[(p->head + p->count) % LO_QUEUE_DEPTH], LO_MSG_MAX + 1, text);
    p->count++; p->sent++; total_sent++;
    irq_restore(f);
    return LO_OK;
}
int lo_recv(uint32_t port, char *buf, size_t cap) {
    if (port < 1 || port > 65535) return LO_ERR_PORT;
    if (!buf || cap == 0) return LO_ERR_LONG;
    uint32_t f = irq_save();
    queue_t *p = find(port, false);
    if (!p || p->count == 0) { irq_restore(f); return LO_ERR_EMPTY; }
    const char *m = p->msg[p->head];
    size_t n = strlen(m);
    copy_text(buf, cap, m);
    p->head = (p->head + 1) % LO_QUEUE_DEPTH; p->count--;
    irq_restore(f);
    return (int)(n < cap ? n : cap - 1);
}
int lo_pending(uint32_t port) { queue_t *p = find(port, false); return p ? (int)p->count : 0; }
bool lo_port_at(size_t i, uint32_t *port, uint32_t *pending, uint32_t *sent) {
    size_t seen = 0;
    for (size_t k = 0; k < LO_MAX_PORTS; k++) if (q[k].port && seen++ == i) { *port = q[k].port; *pending = q[k].count; *sent = q[k].sent; return true; }
    return false;
}
void lo_clear(uint32_t port) {
    uint32_t f = irq_save();
    for (size_t i = 0; i < LO_MAX_PORTS; i++) if (port == 0 || q[i].port == port) { q[i].port = 0; q[i].head = q[i].count = q[i].sent = 0; }
    if (port == 0) total_sent = 0;
    irq_restore(f);
}
uint32_t lo_total_sent(void) { return total_sent; }
const char *lo_strerror(int c) {
    switch (c) {
        case LO_OK: return "ok";
        case LO_ERR_PORT: return "the port must be 1-65535";
        case LO_ERR_FULL: return "the queue is full (8 messages) or all 8 port slots are in use";
        case LO_ERR_LONG: return "the message is longer than 120 characters";
        case LO_ERR_EMPTY: return "no message waiting";
        default: return "unknown loopback error";
    }
}

int lo_echo_step(void) {
    char in[LO_MSG_MAX + 1], out[LO_MSG_MAX + 1];
    int n = lo_recv(LO_ECHO_IN, in, sizeof in);
    if (n < 0) return 0;
    if (!strcmp(in, "quit")) { lo_send(LO_ECHO_OUT, "echo task: bye"); return 2; }
    const char *prefix = "echo: "; size_t w = 0;
    while (prefix[w]) { out[w] = prefix[w]; w++; }
    for (size_t i = 0; in[i] && w < LO_MSG_MAX; i++) out[w++] = in[i];
    out[w] = '\0';
    lo_send(LO_ECHO_OUT, out);
    return 1;
}
