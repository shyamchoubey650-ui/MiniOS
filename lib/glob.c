#include "include/glob.h"

bool glob_match(const char *p, const char *t) {
    const char *star_p = NULL, *star_t = NULL;     /* last '*' seen, and where in the text it started matching */
    if (!p || !t) return false;
    while (*t) {
        if (*p == '*') { star_p = p++; star_t = t; }
        else if (*p == '?' || *p == *t) { p++; t++; }
        else if (star_p) { p = star_p + 1; t = ++star_t; }   /* let the star swallow one more character */
        else return false;
    }
    while (*p == '*') p++;
    return *p == '\0';
}
