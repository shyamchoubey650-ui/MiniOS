#ifndef TEXTUTIL_H
#define TEXTUTIL_H
#include "include/types.h"
/* Pure helpers for head/tail/wc/grep/file (unit-tested on the host). A "line" ends at '\n';
 * a last line without '\n' still counts as a line for head/tail/grep. */
size_t text_head_len(const char *buf, size_t len, size_t lines);      /* bytes occupied by the first `lines` lines */
size_t text_tail_start(const char *buf, size_t len, size_t lines);    /* offset where the last `lines` lines begin */
void   text_counts(const char *buf, size_t len, size_t *lines, size_t *words, size_t *bytes);   /* wc semantics: lines = number of '\n' */
bool   text_is_text(const char *buf, size_t len);                     /* only printable ASCII, tab, CR, LF */
bool   text_contains(const char *hay, size_t len, const char *needle, bool ignore_case);
#endif
