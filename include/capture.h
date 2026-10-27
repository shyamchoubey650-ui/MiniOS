#ifndef CAPTURE_H
#define CAPTURE_H
#include "include/types.h"
/* Output capture for shell redirection ("cmd > file"). While active, characters written to the
 * screen by the OWNER task are appended to a buffer instead of being shown. Other tasks (for
 * example a background `ticker`) keep writing to the screen. Only one capture at a time. */
void   capture_begin(char *buf, size_t capacity, bool (*is_owner)(void));   /* is_owner == NULL: capture everything */
bool   capture_write(char c);            /* true = the character was captured (do not display it) */
size_t capture_end(bool *overflowed);    /* stops capturing; returns bytes captured */
bool   capture_active(void);
#endif
