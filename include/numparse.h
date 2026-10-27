#ifndef NUMPARSE_H
#define NUMPARSE_H
#include "include/types.h"
/* Parses an unsigned 32-bit number: decimal ("123") or hex ("0x1F").
 * Returns false (and leaves *out untouched) on empty input, stray characters,
 * a lone "0x", a sign, or overflow past 4294967295. No whitespace is skipped. */
bool parse_u32(const char *s, uint32_t *out);
#endif
