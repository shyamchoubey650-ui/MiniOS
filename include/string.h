#ifndef STRING_H
#define STRING_H
#include "include/types.h"
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    memcmp(const void *a, const void *b, size_t n);
char  *strchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);
void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
#endif
