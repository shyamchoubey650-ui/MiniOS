#include "include/numparse.h"

bool parse_u32(const char *s, uint32_t *out) {
    uint32_t base = 10, v = 0;
    if (!s || !out || !*s) return false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16; s += 2;
        if (!*s) return false;                       /* "0x" alone */
    }
    for (; *s; s++) {
        uint32_t d;
        char c = *s;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return false;
        if (v > (0xFFFFFFFFu - d) / base) return false;   /* v*base+d would overflow */
        v = v * base + d;
    }
    *out = v;
    return true;
}
