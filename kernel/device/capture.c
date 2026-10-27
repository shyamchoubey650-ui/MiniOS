#include "include/capture.h"

static char   *cap_buf;
static size_t  cap_size, cap_len;
static bool    cap_overflow, cap_on;
static bool  (*cap_owner)(void);

void capture_begin(char *buf, size_t capacity, bool (*is_owner)(void)) {
    cap_buf = buf; cap_size = capacity; cap_len = 0; cap_overflow = false; cap_owner = is_owner; cap_on = buf != NULL && capacity > 0;
}

bool capture_write(char c) {
    if (!cap_on) return false;
    if (cap_owner && !cap_owner()) return false;
    if (cap_len < cap_size) cap_buf[cap_len++] = c;
    else cap_overflow = true;                    /* keep consuming so nothing leaks to the screen */
    return true;
}

size_t capture_end(bool *overflowed) {
    size_t n = cap_len;
    if (overflowed) *overflowed = cap_overflow;
    cap_on = false; cap_buf = NULL; cap_owner = NULL; cap_len = 0; cap_overflow = false;
    return n;
}

bool capture_active(void) { return cap_on; }
