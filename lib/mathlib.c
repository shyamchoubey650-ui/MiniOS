/* Integer math helpers; see include/mathlib.h. No floating point (the kernel has no FPU state handling). */
#include "include/mathlib.h"

/* ---------- calc: recursive descent with overflow checks ---------- */
typedef struct { const char *p; int depth; int err; } calc_t;

static void skip(calc_t *c) { while (*c->p == ' ' || *c->p == '\t') c->p++; }
static int64_t expr(calc_t *c);

/* The kernel is linked without libgcc, so 64-bit division and overflow-checking multiplication are done by hand. */
static uint64_t udivmod(uint64_t n, uint64_t d, uint64_t *rem) {     /* d != 0; shift-and-subtract */
    if ((n >> 32) == 0 && (d >> 32) == 0) {                              /* fast path: the CPU divides 32-bit values natively */
        uint32_t a = (uint32_t)n, b = (uint32_t)d;
        if (rem) *rem = a % b;
        return a / b;
    }
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= (uint64_t)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}
static uint64_t udiv(uint64_t n, uint64_t d) { return udivmod(n, d, NULL); }
static uint64_t umod(uint64_t n, uint64_t d) { uint64_t r; udivmod(n, d, &r); return r; }

static bool add_ovf(int64_t a, int64_t b, int64_t *r) {
    uint64_t s = (uint64_t)a + (uint64_t)b;
    *r = (int64_t)s;
    return ((a >= 0) == (b >= 0)) && ((*r >= 0) != (a >= 0));
}
static bool sub_ovf(int64_t a, int64_t b, int64_t *r) {
    uint64_t s = (uint64_t)a - (uint64_t)b;
    *r = (int64_t)s;
    return ((a >= 0) != (b >= 0)) && ((*r >= 0) != (a >= 0));
}
static bool mul_ovf(int64_t a, int64_t b, int64_t *r) {
    uint64_t ua = a < 0 ? 0 - (uint64_t)a : (uint64_t)a, ub = b < 0 ? 0 - (uint64_t)b : (uint64_t)b;
    uint64_t prod = ua * ub;
    if (ua != 0 && udiv(prod, ua) != ub) return true;                    /* the unsigned product wrapped */
    bool neg = (a < 0) != (b < 0);
    if (neg ? prod > (uint64_t)1 << 63 : prod > ((uint64_t)1 << 63) - 1) return true;
    *r = neg ? (int64_t)(0 - prod) : (int64_t)prod;
    return false;
}

static int64_t primary(calc_t *c) {
    skip(c);
    if (c->err) return 0;
    if (*c->p == '(') {
        if (++c->depth > 32) { c->err = CALC_ERR_DEPTH; return 0; }
        c->p++;
        int64_t v = expr(c);
        skip(c);
        if (c->err) return 0;
        if (*c->p != ')') { c->err = CALC_ERR_SYNTAX; return 0; }
        c->p++; c->depth--;
        return v;
    }
    if (*c->p == '-' || *c->p == '+') {
        bool neg = *c->p == '-'; c->p++;
        if (++c->depth > 32) { c->err = CALC_ERR_DEPTH; return 0; }
        int64_t v = primary(c);
        c->depth--;
        if (c->err) return 0;
        if (neg && v == INT64_MIN) { c->err = CALC_ERR_OVERFLOW; return 0; }
        return neg ? -v : v;
    }
    if (*c->p < '0' || *c->p > '9') { c->err = CALC_ERR_SYNTAX; return 0; }
    int64_t v = 0;
    while (*c->p >= '0' && *c->p <= '9') {
        int64_t t;
        if (mul_ovf(v, 10, &t) || add_ovf(t, *c->p - '0', &v)) { c->err = CALC_ERR_OVERFLOW; return 0; }
        c->p++;
    }
    return v;
}

static int64_t term(calc_t *c) {
    int64_t v = primary(c);
    for (;;) {
        skip(c);
        char op = *c->p;
        if (c->err || (op != '*' && op != '/' && op != '%')) return v;
        c->p++;
        int64_t r = primary(c);
        if (c->err) return 0;
        if (op == '*') { if (mul_ovf(v, r, &v)) { c->err = CALC_ERR_OVERFLOW; return 0; } }
        else if (r == 0) { c->err = CALC_ERR_DIV0; return 0; }
        else if (v == INT64_MIN && r == -1) { c->err = CALC_ERR_OVERFLOW; return 0; }
        else { uint64_t ua = v < 0 ? 0 - (uint64_t)v : (uint64_t)v, ub = r < 0 ? 0 - (uint64_t)r : (uint64_t)r, rem;
               uint64_t q = udivmod(ua, ub, &rem);                       /* truncating division; the remainder takes the dividend's sign */
               v = op == '/' ? ((v < 0) != (r < 0) ? (int64_t)(0 - q) : (int64_t)q) : (v < 0 ? (int64_t)(0 - rem) : (int64_t)rem); }
    }
}

static int64_t expr(calc_t *c) {
    int64_t v = term(c);
    for (;;) {
        skip(c);
        char op = *c->p;
        if (c->err || (op != '+' && op != '-')) return v;
        c->p++;
        int64_t r = term(c);
        if (c->err) return 0;
        if (op == '+' ? add_ovf(v, r, &v) : sub_ovf(v, r, &v)) { c->err = CALC_ERR_OVERFLOW; return 0; }
    }
}

int calc_eval(const char *s, int64_t *out) {
    calc_t c = { s, 0, 0 };
    int64_t v = expr(&c);
    skip(&c);
    if (!c.err && *c.p != '\0') c.err = CALC_ERR_SYNTAX;
    if (c.err) return c.err;
    *out = v;
    return CALC_OK;
}

const char *calc_strerror(int code) {
    switch (code) {
        case CALC_OK: return "ok";
        case CALC_ERR_SYNTAX: return "syntax error";
        case CALC_ERR_DIV0: return "division by zero";
        case CALC_ERR_OVERFLOW: return "result does not fit in 64 bits";
        case CALC_ERR_DEPTH: return "expression nested too deeply";
        default: return "unknown error";
    }
}

bool math_add_ovf(int64_t a, int64_t b, int64_t *r) { return add_ovf(a, b, r); }
void math_idivmod(int64_t a, int64_t b, int64_t *q, int64_t *r) {
    uint64_t ua = a < 0 ? 0 - (uint64_t)a : (uint64_t)a, ub = b < 0 ? 0 - (uint64_t)b : (uint64_t)b, rem;
    uint64_t qq = udivmod(ua, ub, &rem);
    *q = ((a < 0) != (b < 0)) ? (int64_t)(0 - qq) : (int64_t)qq;
    *r = a < 0 ? (int64_t)(0 - rem) : (int64_t)rem;
}

/* ---------- number theory ---------- */
uint64_t math_gcd(uint64_t a, uint64_t b) { while (b) { uint64_t t = umod(a, b); a = b; b = t; } return a; }

bool math_is_prime(uint64_t n) {
    if (n < 2) return false;
    if (n < 4) return true;
    if (umod(n, 2) == 0 || umod(n, 3) == 0) return false;
    for (uint64_t i = 5; i <= udiv(n, i); i += 6) if (umod(n, i) == 0 || umod(n, i + 2) == 0) return false;
    return true;
}

int math_factor(uint64_t n, uint64_t *out, int max) {
    int k = 0;
    if (n < 2) return 0;
    for (uint64_t p = 2; p <= udiv(n, p); p += (p == 2 ? 1 : 2)) {
        while (umod(n, p) == 0) { if (k >= max) return k; out[k++] = p; n = udiv(n, p); }
    }
    if (n > 1 && k < max) out[k++] = n;
    return k;
}

bool math_fib(unsigned n, uint64_t *out) {
    if (n > 93) return false;
    uint64_t a = 0, b = 1;
    for (unsigned i = 0; i < n; i++) { uint64_t t = a + b; a = b; b = t; }
    *out = a;
    return true;
}

size_t math_to_base(uint64_t v, unsigned base, char *buf, size_t cap) {
    if (base < 2 || base > 36 || cap < 2) return 0;
    char tmp[65]; size_t n = 0;
    do { unsigned d = (unsigned)umod(v, base); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v = udiv(v, base); } while (v);
    if (n + 1 > cap) return 0;
    for (size_t i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

bool math_from_base(const char *s, unsigned base, uint64_t *out) {
    if (base < 2 || base > 36 || !*s) return false;
    uint64_t v = 0;
    for (; *s; s++) {
        unsigned d;
        if (*s >= '0' && *s <= '9') d = (unsigned)(*s - '0');
        else if (*s >= 'a' && *s <= 'z') d = (unsigned)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'Z') d = (unsigned)(*s - 'A' + 10);
        else return false;
        if (d >= base) return false;
        if (v > udiv(UINT64_MAX - d, base)) return false;
        v = v * base + d;
    }
    *out = v;
    return true;
}

/* ---------- random ---------- */
uint32_t rand_next(uint32_t *st) { *st = *st * 1664525u + 1013904223u; return *st; }
uint32_t rand_range(uint32_t *st, uint32_t n) {
    if (n <= 1) return 0;
    if (n > 0x01000000u) return rand_next(st) % n;                      /* huge ranges: slight bias, still fine for a demo */
    uint32_t limit = 0x01000000u - 0x01000000u % n, v;                  /* use the high 24 bits (an LCG's low bits are weak) and reject the biased tail */
    do { v = rand_next(st) >> 8; } while (v >= limit);
    return v % n;
}

/* ---------- calendar ---------- */
bool cal_leap(unsigned y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
unsigned cal_days_in_month(unsigned y, unsigned m) {
    static const unsigned d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1 || m > 12) return 0;
    return (m == 2 && cal_leap(y)) ? 29 : d[m - 1];
}
unsigned cal_weekday(unsigned y, unsigned m, unsigned d) {
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (unsigned)((y + y / 4 - y / 100 + y / 400 + (unsigned)t[m - 1] + d) % 7);
}
