#ifndef MATHLIB_H
#define MATHLIB_H
/* Pure integer helpers for calc, factor, prime, fib, gcd, base, random, cal (unit-tested on the host). */
#include "include/types.h"

#define CALC_OK            0
#define CALC_ERR_SYNTAX  (-1)
#define CALC_ERR_DIV0    (-2)
#define CALC_ERR_OVERFLOW (-3)
#define CALC_ERR_DEPTH   (-4)
/* Evaluates + - * / % ( ) and unary +/-, with the usual precedence, on 64-bit integers.
 * Division truncates toward zero. Every operation is overflow-checked. */
int  calc_eval(const char *expr, int64_t *out);
const char *calc_strerror(int code);

bool     math_add_ovf(int64_t a, int64_t b, int64_t *r);               /* true on overflow; *r holds the wrapped value */
void     math_idivmod(int64_t a, int64_t b, int64_t *q, int64_t *r);   /* b != 0; truncating, remainder has the sign of a; a=INT64_MIN,b=-1 wraps */
uint64_t math_gcd(uint64_t a, uint64_t b);
bool     math_is_prime(uint64_t n);                           /* trial division; fine for 32-bit values */
int      math_factor(uint64_t n, uint64_t *out, int max);     /* prime factors in ascending order with repeats; returns the count */
bool     math_fib(unsigned n, uint64_t *out);                 /* false if F(n) does not fit in 64 bits (n > 93) */
size_t   math_to_base(uint64_t v, unsigned base, char *buf, size_t cap);   /* 2..36, lowercase; returns the length, 0 if cap is too small */
bool     math_from_base(const char *s, unsigned base, uint64_t *out);       /* digits only; false on overflow or bad digit */

uint32_t rand_next(uint32_t *state);                          /* LCG (Numerical Recipes constants): NOT secure */
uint32_t rand_range(uint32_t *state, uint32_t n);             /* 0..n-1 without modulo bias; n > 0 */

bool     cal_leap(unsigned year);
unsigned cal_days_in_month(unsigned year, unsigned month);    /* month 1..12 */
unsigned cal_weekday(unsigned year, unsigned month, unsigned day);   /* 0 = Sunday */
#endif
