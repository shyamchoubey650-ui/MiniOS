/* calc, number theory, base conversion, random, calendar, and the shell variable/alias library. */
#include <stdio.h>
#include <string.h>
#include "include/mathlib.h"
#include "include/shellvars.h"

static int run, failed;
#define CHECK(c) do { run++; if (!(c)) { failed++; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static bool ev(const char *e, int64_t want) { int64_t v; return calc_eval(e, &v) == CALC_OK && v == want; }
static int err(const char *e) { int64_t v; return calc_eval(e, &v); }

int main(void) {
    puts("calc: precedence, parentheses, signs");
    CHECK(ev("1+2*3", 7) && ev("(1+2)*3", 9) && ev("10-4-3", 3) && ev("2*3+4*5", 26) && ev(" 7 ", 7) && ev("-5+3", -2) && ev("--5", 5) && ev("+-+5", -5));
    CHECK(ev("100/7", 14) && ev("-100/7", -14) && ev("100/-7", -14) && ev("100%7", 2) && ev("-100%7", -2) && ev("100%-7", 2) && ev("7/7/7", 0) && ev("((((1))))", 1));
    CHECK(ev("9223372036854775807", INT64_MAX) && ev("-9223372036854775807-1", INT64_MIN) && ev("3037000499*3037000499", 9223372030926249001LL));
    puts("calc: errors are reported, never silent");
    CHECK(err("1/0") == CALC_ERR_DIV0 && err("5%0") == CALC_ERR_DIV0 && err("1/(2-2)") == CALC_ERR_DIV0);
    CHECK(err("") == CALC_ERR_SYNTAX && err("1+") == CALC_ERR_SYNTAX && err("(1") == CALC_ERR_SYNTAX && err("1)") == CALC_ERR_SYNTAX && err("1 2") == CALC_ERR_SYNTAX && err("*3") == CALC_ERR_SYNTAX && err("2x") == CALC_ERR_SYNTAX);
    CHECK(err("9223372036854775808") == CALC_ERR_OVERFLOW && err("9223372036854775807+1") == CALC_ERR_OVERFLOW && err("-9223372036854775807-2") == CALC_ERR_OVERFLOW);
    CHECK(err("4294967296*4294967296") == CALC_ERR_OVERFLOW && err("3037000500*3037000500") == CALC_ERR_OVERFLOW && err("(-9223372036854775807-1)/-1") == CALC_ERR_OVERFLOW);
    CHECK(err("-(-9223372036854775807-1)") == CALC_ERR_OVERFLOW);
    char deep[256]; memset(deep, '(', 100); deep[100] = '1'; memset(deep + 101, ')', 100); deep[201] = 0; CHECK(err(deep) == CALC_ERR_DEPTH);
    CHECK(!strcmp(calc_strerror(CALC_ERR_DIV0), "division by zero"));

    { int64_t q, r, t; math_idivmod(-7, 2, &q, &r); CHECK(q == -3 && r == -1); math_idivmod(7, -2, &q, &r); CHECK(q == -3 && r == 1); math_idivmod(9, 3, &q, &r); CHECK(q == 3 && r == 0);
      CHECK(!math_add_ovf(1, 2, &t) && t == 3 && math_add_ovf(INT64_MAX, 1, &t) && math_add_ovf(INT64_MIN, -1, &t) && !math_add_ovf(INT64_MAX, INT64_MIN, &t)); }
    puts("number theory");
    CHECK(math_gcd(12, 18) == 6 && math_gcd(17, 5) == 1 && math_gcd(0, 9) == 9 && math_gcd(9, 0) == 9 && math_gcd(0, 0) == 0 && math_gcd(1ull << 40, 1ull << 20) == 1ull << 20);
    CHECK(!math_is_prime(0) && !math_is_prime(1) && math_is_prime(2) && math_is_prime(3) && !math_is_prime(4) && math_is_prime(97) && !math_is_prime(91) && math_is_prime(2147483647u) && !math_is_prime(4294967295u) && math_is_prime(4294967291u));
    int primes = 0; for (unsigned i = 0; i < 1000; i++) primes += math_is_prime(i); CHECK(primes == 168);
    uint64_t f[64]; int n = math_factor(360, f, 64); CHECK(n == 6 && f[0] == 2 && f[1] == 2 && f[2] == 2 && f[3] == 3 && f[4] == 3 && f[5] == 5);
    n = math_factor(97, f, 64); CHECK(n == 1 && f[0] == 97); CHECK(math_factor(1, f, 64) == 0 && math_factor(0, f, 64) == 0);
    n = math_factor(4294967295u, f, 64); CHECK(n == 5 && f[0] == 3 && f[1] == 5 && f[2] == 17 && f[3] == 257 && f[4] == 65537);
    n = math_factor(1ull << 40, f, 64); CHECK(n == 40); CHECK(math_factor(1ull << 40, f, 10) == 10);          /* the cap is honoured */
    uint64_t x; CHECK(math_fib(0, &x) && x == 0 && math_fib(1, &x) && x == 1 && math_fib(10, &x) && x == 55 && math_fib(93, &x) && x == 12200160415121876738ull && !math_fib(94, &x));
    puts("bases");
    char b[80];
    CHECK(math_to_base(255, 16, b, sizeof b) == 2 && !strcmp(b, "ff") && math_to_base(0, 2, b, sizeof b) == 1 && !strcmp(b, "0") && math_to_base(5, 2, b, sizeof b) == 3 && !strcmp(b, "101"));
    CHECK(math_to_base(UINT64_MAX, 2, b, sizeof b) == 64 && math_to_base(UINT64_MAX, 2, b, 64) == 0 && math_to_base(35, 36, b, sizeof b) == 1 && !strcmp(b, "z") && math_to_base(1, 1, b, sizeof b) == 0 && math_to_base(1, 37, b, sizeof b) == 0);
    CHECK(math_from_base("ff", 16, &x) && x == 255 && math_from_base("FF", 16, &x) && x == 255 && math_from_base("101", 2, &x) && x == 5 && math_from_base("zz", 36, &x) && x == 1295);
    CHECK(!math_from_base("2", 2, &x) && !math_from_base("", 10, &x) && !math_from_base("12x", 10, &x) && !math_from_base("-1", 10, &x) && !math_from_base("18446744073709551616", 10, &x) && math_from_base("18446744073709551615", 10, &x) && x == UINT64_MAX);

    puts("random");
    uint32_t st = 1, a = rand_next(&st), c = rand_next(&st); st = 1; CHECK(rand_next(&st) == a && rand_next(&st) == c && a != c);   /* deterministic */
    int bucket[6] = {0}; st = 12345; for (int i = 0; i < 6000; i++) { uint32_t r = rand_range(&st, 6); CHECK(r < 6); bucket[r]++; }
    for (int i = 0; i < 6; i++) CHECK(bucket[i] > 800 && bucket[i] < 1200);                            /* roughly uniform */
    CHECK(rand_range(&st, 1) == 0 && rand_range(&st, 0) == 0);

    puts("calendar");
    CHECK(cal_leap(2024) && !cal_leap(2100) && cal_leap(2000) && !cal_leap(2026));
    CHECK(cal_days_in_month(2026, 2) == 28 && cal_days_in_month(2024, 2) == 29 && cal_days_in_month(2026, 12) == 31 && cal_days_in_month(2026, 4) == 30 && cal_days_in_month(2026, 13) == 0 && cal_days_in_month(2026, 0) == 0);
    CHECK(cal_weekday(2026, 10, 12) == 1 && cal_weekday(2000, 1, 1) == 6 && cal_weekday(1970, 1, 1) == 4 && cal_weekday(2024, 2, 29) == 4);   /* Mon, Sat, Thu, Thu */

    puts("shell variables");
    sv_reset();
    CHECK(var_set("NAME", "MiniOS", false) == SV_OK && !strcmp(var_get("NAME"), "MiniOS") && var_get("NOPE") == NULL && var_count() == 1);
    CHECK(var_set("1bad", "x", false) == SV_ERR_NAME && var_set("", "x", false) == SV_ERR_NAME && var_set("a-b", "x", false) == SV_ERR_NAME && var_set("abcdefghijklmnop", "x", false) == SV_ERR_NAME && var_set(NULL, "x", false) == SV_ERR_NAME);
    char big[100]; memset(big, 'v', 99); big[99] = 0; CHECK(var_set("BIG", big, false) == SV_ERR_VALUE);
    CHECK(var_set("A", "1", false) == SV_OK && var_set("A", "2", false) == SV_OK && !strcmp(var_get("A"), "2") && var_count() == 2);   /* overwrite, not duplicate */
    const char *nm, *vl; bool ex;
    CHECK(var_at(0, &nm, &vl, &ex) && !strcmp(nm, "A") && !ex && var_at(1, &nm, &vl, &ex) && !strcmp(nm, "NAME") && !var_at(2, &nm, &vl, &ex));      /* sorted */
    CHECK(var_export("A") == SV_OK && var_at(0, &nm, &vl, &ex) && ex && var_set("A", "3", false) == SV_OK && var_at(0, &nm, &vl, &ex) && ex);        /* export sticks */
    CHECK(var_export("ZZ") == SV_ERR_NOT_FOUND && var_unset("ZZ") == SV_ERR_NOT_FOUND && var_unset("A") == SV_OK && var_get("A") == NULL);
    for (int i = 0; i < VAR_MAX; i++) { char n[8]; snprintf(n, sizeof n, "V%d", i); var_set(n, "x", false); }
    CHECK(var_set("OVERFLOW", "x", false) == SV_ERR_FULL && var_count() == VAR_MAX);
    sv_reset(); var_set("USER", "student", false); var_set("N", "5", false); var_set("?", "0", false); char o[64];
    puts("expansion");
    CHECK(var_set("?", "0", false) == SV_ERR_NAME);                                                    /* "?" is only a special NAME for expansion; sv_reset cleared it */
    CHECK(sv_expand_vars("hi $USER", o, sizeof o) == SV_OK && !strcmp(o, "hi student"));
    CHECK(sv_expand_vars("${USER}x $N$N", o, sizeof o) == SV_OK && !strcmp(o, "studentx 55"));
    CHECK(sv_expand_vars("$MISSING|", o, sizeof o) == SV_OK && !strcmp(o, "|"));
    CHECK(sv_expand_vars("cost $ 5 and $", o, sizeof o) == SV_OK && !strcmp(o, "cost $ 5 and $"));
    CHECK(sv_expand_vars("'$USER' \"$USER\"", o, sizeof o) == SV_OK && !strcmp(o, "'$USER' \"student\""));   /* single quotes block expansion */
    sv_set_status(127); CHECK(sv_expand_vars("rc=$? ${?}", o, sizeof o) == SV_OK && !strcmp(o, "rc=127 127")); sv_set_status(0);
    CHECK(sv_expand_vars("${USER", o, sizeof o) == SV_OK && !strcmp(o, "${USER"));
    CHECK(sv_expand_vars("$USER$USER$USER", o, 20) == SV_ERR_SPACE && sv_expand_vars("abcdef", o, 6) == SV_ERR_SPACE && sv_expand_vars("abcde", o, 6) == SV_OK);
    puts("aliases");
    bool ch;
    CHECK(alias_set("ll", "ls -l") == SV_OK && !strcmp(alias_get("ll"), "ls -l") && alias_get("zz") == NULL && alias_count() == 1);
    CHECK(sv_expand_alias("ll /tmp", o, sizeof o, &ch) == SV_OK && ch && !strcmp(o, "ls -l /tmp"));
    CHECK(sv_expand_alias("  ll", o, sizeof o, &ch) == SV_OK && ch && !strcmp(o, "  ls -l"));
    CHECK(sv_expand_alias("echo ll", o, sizeof o, &ch) == SV_OK && !ch && !strcmp(o, "echo ll"));       /* only the first word */
    CHECK(sv_expand_alias("llx", o, sizeof o, &ch) == SV_OK && !ch);
    CHECK(alias_set("ls", "ls -l") == SV_OK && sv_expand_alias("ls", o, sizeof o, &ch) == SV_OK && !strcmp(o, "ls -l"));   /* expanded once: no endless loop */
    CHECK(alias_set("bad name", "x") == SV_ERR_NAME && alias_set("x", "") == SV_ERR_VALUE && alias_remove("nope") == SV_ERR_NOT_FOUND && alias_remove("ll") == SV_OK && alias_get("ll") == NULL);
    CHECK(sv_expand_alias("ls aaaaaaaaaaaaaaaa", o, 10, &ch) == SV_ERR_SPACE);
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
