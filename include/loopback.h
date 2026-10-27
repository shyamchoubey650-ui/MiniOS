#ifndef LOOPBACK_H
#define LOOPBACK_H
/* Loopback message queue (Phase 12): REAL, in-memory, between tasks.
 *
 * Messages sent to a port are stored in kernel memory until somebody receives them: a command in the shell and a task
 * started with `spawn lbecho` really do exchange messages through it. Nothing here touches a network card.
 * Rules: ports 1-65535; at most 8 ports have queues at once; each queue holds 8 messages of up to 120 characters;
 * messages are delivered oldest first. As on Unix, receiving on a port below 1024 is for root only (the commands enforce it);
 * anybody may send. Messages are not private: any user who can receive on a port sees what was sent to it. */
#include "include/types.h"

#define LO_MAX_PORTS   8
#define LO_QUEUE_DEPTH 8
#define LO_MSG_MAX     120
#define LO_ECHO_IN     7777     /* the echo task listens here ... */
#define LO_ECHO_OUT    7778     /* ... and answers here (both above 1023: any user may receive) */

#define LO_OK          0
#define LO_ERR_PORT   (-1)      /* not 1-65535 */
#define LO_ERR_FULL   (-2)      /* queue full, or all 8 port slots are in use */
#define LO_ERR_LONG   (-3)
#define LO_ERR_EMPTY  (-4)

int      lo_send(uint32_t port, const char *text);
int      lo_recv(uint32_t port, char *buf, size_t cap);   /* length >= 0, or LO_ERR_*; the text is cut to fit and always terminated */
int      lo_pending(uint32_t port);                       /* messages waiting (0 for a port with no queue) */
bool     lo_port_at(size_t i, uint32_t *port, uint32_t *pending, uint32_t *sent_total);   /* the i-th port that has a queue */
void     lo_clear(uint32_t port);                         /* 0 = every port */
uint32_t lo_total_sent(void);
const char *lo_strerror(int code);

/* The echo task: handles ONE message if there is one. Returns 1 if it handled a message, 2 if it saw "quit", 0 if idle. */
int      lo_echo_step(void);
#endif
