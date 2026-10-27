#include "include/textutil.h"

size_t text_head_len(const char *buf, size_t len, size_t lines) {
    size_t i = 0;
    while (lines > 0 && i < len) {
        while (i < len && buf[i] != '\n') i++;
        if (i < len) i++;                          /* include the newline */
        lines--;
    }
    return i;
}

size_t text_tail_start(const char *buf, size_t len, size_t lines) {
    if (lines == 0) return len;
    size_t end = len;
    if (end > 0 && buf[end - 1] == '\n') end--;    /* the final newline terminates the last line, it does not start a new one */
    size_t seen = 0;
    while (end > 0) {
        end--;
        if (buf[end] == '\n' && ++seen == lines) return end + 1;
    }
    return 0;
}

void text_counts(const char *buf, size_t len, size_t *lines, size_t *words, size_t *bytes) {
    size_t l = 0, w = 0; bool in_word = false;
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n') l++;
        bool space = c == ' ' || c == '\n' || c == '\t' || c == '\r';
        if (!space && !in_word) w++;
        in_word = !space;
    }
    if (lines) *lines = l;
    if (words) *words = w;
    if (bytes) *bytes = len;
}

bool text_is_text(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\n' || c == '\r' || c == '\t') continue;
        if (c < 32 || c > 126) return false;
    }
    return true;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool text_contains(const char *hay, size_t len, const char *needle, bool ic) {
    size_t n = 0;
    if (!hay || !needle) return false;
    while (needle[n]) n++;
    if (n == 0) return true;
    if (n > len) return false;
    for (size_t i = 0; i + n <= len; i++) {
        size_t k = 0;
        while (k < n && (ic ? lower(hay[i + k]) == lower(needle[k]) : hay[i + k] == needle[k])) k++;
        if (k == n) return true;
    }
    return false;
}
