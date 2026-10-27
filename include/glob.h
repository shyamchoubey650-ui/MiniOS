#ifndef GLOB_H
#define GLOB_H
#include "include/types.h"
/* Wildcard match: '*' = any run of characters (including none), '?' = exactly one character.
 * Case-sensitive, no brackets, no escapes. */
bool glob_match(const char *pattern, const char *text);
#endif
