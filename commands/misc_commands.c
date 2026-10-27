/* Utilities (Phase 11): calc cal random base color timer stopwatch factor prime fib gcd seq yes dice cowsay demo credits.
 * Integer math only (lib/mathlib.c). random uses a linear congruential generator: fine for dice, NOT for security. */
#include "include/builtins.h"
#include "include/cmdutil.h"
#include "include/screen.h"
#include "include/syscall.h"
#include "include/mathlib.h"
#include "include/kinfo.h"
#include "include/timer.h"
#include "include/version.h"
#include "include/numparse.h"
#include "include/string.h"

static bool join_args(int argc, const char **argv, int from, char *out, size_t cap) {
    size_t o = 0;
    for (int i = from; i < argc; i++) {
        size_t l = strlen(argv[i]);
        if (o + l + 2 > cap) return false;
        if (i > from) out[o++] = ' ';
        memcpy(out + o, argv[i], l); o += l;
    }
    out[o] = '\0';
    return true;
}
static bool parse_u64(const char *s, uint64_t *v) { return math_from_base(s, 10, v); }

/* ---------- calc ---------- */
static int cmd_calc(int argc, const char **argv) {
    char text[128]; int64_t v;
    if (argc < 2) return cu_usage("calc <expression>     e.g. calc 2*(3+4)   (integers, + - * / % and parentheses)");
    if (!join_args(argc, argv, 1, text, sizeof text)) return cu_error("calc", "expression too long.");
    int rc = calc_eval(text, &v);
    if (rc != CALC_OK) return cu_error("calc", calc_strerror(rc));
    cu_put_i64(v); screen_putc('\n');
    return 0;
}

/* ---------- calendar ---------- */
static int cmd_cal(int argc, const char **argv) {
    unsigned month, year, today = 0;
    if (argc == 1) {
        ki_rtc_t r; int rc = sys_kinfo(KI_RTC, 0, &r, sizeof r);
        if (rc != 0) return cu_error("cal", sys_strerror(rc));
        month = r.month; year = r.year; today = r.day;
    } else if (argc == 3) {
        uint32_t m, y;
        if (!cu_parse_count(argv[1], &m, 12) || m == 0 || !cu_parse_count(argv[2], &y, 9999) || y == 0) return cu_error("cal", "month is 1-12 and year is 1-9999.");
        month = m; year = y;
    } else return cu_usage("cal [month year]     e.g. cal 2 2024");
    static const char *const names[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    screen_puts("   "); screen_puts(names[month - 1]); screen_putc(' '); screen_put_dec(year); screen_putc('\n');
    screen_puts("Su Mo Tu We Th Fr Sa\n");
    unsigned wd = cal_weekday(year, month, 1), days = cal_days_in_month(year, month);
    for (unsigned i = 0; i < wd; i++) screen_puts("   ");
    for (unsigned d = 1; d <= days; d++) {
        if (d == today) cu_color(VGA_LIGHT_GREEN);
        if (d < 10) screen_putc(' ');
        screen_put_dec(d);
        if (d == today) cu_color(VGA_LIGHT_GREY);
        screen_putc(((wd + d) % 7 == 0 || d == days) ? '\n' : ' ');
    }
    return 0;
}

/* ---------- number tools ---------- */
static uint32_t rng_state; static bool rng_seeded;
static uint32_t *rng(void) { if (!rng_seeded) { rng_state = sys_time() * 2654435761u + 12345u; rng_seeded = true; } return &rng_state; }

static int cmd_random(int argc, const char **argv) {
    uint32_t lo = 1, hi = 100;
    if (argc == 2) { if (!parse_u32(argv[1], &hi)) return cu_error("random", "limit must be a number."); }
    else if (argc == 3) { if (!parse_u32(argv[1], &lo) || !parse_u32(argv[2], &hi)) return cu_error("random", "limits must be numbers."); }
    else if (argc != 1) return cu_usage("random [max]  |  random <min> <max>     (inclusive; default 1 to 100)");
    if (lo > hi) return cu_error("random", "min is larger than max.");
    if (lo == 0 && hi == 0xFFFFFFFFu) { screen_put_dec(rand_next(rng())); screen_putc('\n'); return 0; }
    screen_put_dec(lo + rand_range(rng(), hi - lo + 1)); screen_putc('\n');
    return 0;
}

static int cmd_base(int argc, const char **argv) {
    uint64_t v; uint32_t from = 10, to = 0; char buf[72];
    if (argc < 2 || argc > 4) return cu_usage("base <number> [from [to]]     e.g. base ff 16 10    (bases 2-36; without 'to' shows 2, 8, 10 and 16)");
    if (argc >= 3 && (!cu_parse_count(argv[2], &from, 36) || from < 2)) return cu_error("base", "bases are 2 to 36.");
    if (argc == 4 && (!cu_parse_count(argv[3], &to, 36) || to < 2)) return cu_error("base", "bases are 2 to 36.");
    if (!math_from_base(argv[1], from, &v)) return cu_error("base", "that is not a valid number in that base (or it exceeds 64 bits).");
    static const unsigned all[] = { 2, 8, 10, 16 };
    for (size_t i = 0; i < 4; i++) {
        if (to && all[i] != to) continue;
        math_to_base(v, all[i], buf, sizeof buf);
        cu_pad(all[i] == 2 ? "binary" : all[i] == 8 ? "octal" : all[i] == 10 ? "decimal" : "hex", 8); screen_puts(buf); screen_putc('\n');
    }
    if (to && to != 2 && to != 8 && to != 10 && to != 16) { math_to_base(v, to, buf, sizeof buf); screen_puts("base "); screen_put_dec(to); screen_puts(": "); screen_puts(buf); screen_putc('\n'); }
    return 0;
}

static int cmd_factor(int argc, const char **argv) {
    uint64_t n, f[64];
    if (argc != 2) return cu_usage("factor <number up to 4294967295>");
    if (!parse_u64(argv[1], &n)) return cu_error("factor", "not a number.");
    if (n > 0xFFFFFFFFull) return cu_error("factor", "numbers above 4294967295 would take too long (the kernel has no fast 64-bit division).");
    int k = math_factor(n, f, 64);
    cu_put_u64(n); screen_putc(':');
    if (k == 0) screen_puts(n < 2 ? " (no prime factors)" : "");
    for (int i = 0; i < k; i++) { screen_putc(' '); cu_put_u64(f[i]); }
    screen_putc('\n');
    return 0;
}
static int cmd_prime(int argc, const char **argv) {
    uint64_t n;
    if (argc != 2) return cu_usage("prime <number up to 4294967295>");
    if (!parse_u64(argv[1], &n)) return cu_error("prime", "not a number.");
    if (n > 0xFFFFFFFFull) return cu_error("prime", "numbers above 4294967295 would take too long.");
    cu_put_u64(n); screen_puts(math_is_prime(n) ? " is prime\n" : " is not prime\n");
    return math_is_prime(n) ? 0 : 1;
}
static int cmd_fib(int argc, const char **argv) {
    uint32_t n; uint64_t v;
    if (argc != 2) return cu_usage("fib <n 0-93>     (the n-th Fibonacci number)");
    if (!cu_parse_count(argv[1], &n, 1000)) return cu_error("fib", "n must be a number.");
    if (!math_fib(n, &v)) return cu_error("fib", "F(n) does not fit in 64 bits above n = 93.");
    cu_put_u64(v); screen_putc('\n');
    return 0;
}
static int cmd_gcd(int argc, const char **argv) {
    uint64_t g = 0, v;
    if (argc < 3 || argc > 10) return cu_usage("gcd <a> <b> [c ...]");
    for (int i = 1; i < argc; i++) { if (!parse_u64(argv[i], &v)) return cu_error("gcd", "all arguments must be non-negative numbers."); g = math_gcd(g, v); }
    cu_put_u64(g); screen_putc('\n');
    return 0;
}
static int cmd_seq(int argc, const char **argv) {
    int64_t a = 1, b, step = 1, v[3]; int n = 0;
    if (argc < 2 || argc > 4) return cu_usage("seq <last>  |  seq <first> <last>  |  seq <first> <step> <last>");
    for (int i = 1; i < argc; i++) if (calc_eval(argv[i], &v[n++]) != CALC_OK) return cu_error("seq", "arguments must be integers.");
    if (n == 1) b = v[0]; else if (n == 2) { a = v[0]; b = v[1]; } else { a = v[0]; step = v[1]; b = v[2]; }
    if (step == 0) return cu_error("seq", "step must not be zero.");
    int64_t span = step > 0 ? b - a : a - b, mag = step > 0 ? step : -step, count, rem;
    if (span < 0) return 0;
    math_idivmod(span, mag, &count, &rem);
    if (count >= 1000) return cu_error("seq", "that would print more than 1000 numbers.");
    for (int64_t i = 0, x = a; i <= count; i++, x += step) { if (i) screen_putc(' '); cu_put_i64(x); }
    screen_putc('\n');
    return 0;
}

/* ---------- fun ---------- */
static int cmd_yes(int argc, const char **argv) {
    char text[64] = "y"; uint32_t n = 10; int last = argc;
    if (argc >= 3 && cu_parse_count(argv[argc - 1], &n, 1000)) last = argc - 1;       /* a trailing number is the repeat count */
    else if (argc == 2 && cu_parse_count(argv[1], &n, 1000)) last = 1;               /* "yes 5" repeats the default word */
    if (last > 1 && !join_args(last, argv, 1, text, sizeof text)) return cu_error("yes", "text too long.");
    for (uint32_t i = 0; i < n; i++) { screen_puts(text); screen_putc('\n'); }
    return 0;
}
static int cmd_dice(int argc, const char **argv) {
    uint32_t count = 1, sides = 6;
    if (argc > 3 || (argc >= 2 && (!cu_parse_count(argv[1], &count, 10) || count == 0)) || (argc == 3 && (!cu_parse_count(argv[2], &sides, 1000) || sides < 2))) return cu_usage("dice [count 1-10] [sides 2-1000]     (default: one six-sided die)");
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; i++) { uint32_t r = 1 + rand_range(rng(), sides); total += r; if (i) screen_putc(' '); screen_put_dec(r); }
    if (count > 1) { screen_puts("   total "); screen_put_dec(total); }
    screen_putc('\n');
    return 0;
}
static int cmd_cowsay(int argc, const char **argv) {
    char text[60];
    if (argc < 2) return cu_usage("cowsay <text...>");
    if (!join_args(argc, argv, 1, text, sizeof text)) return cu_error("cowsay", "text too long (limit 58 characters).");
    size_t n = strlen(text);
    screen_putc(' '); for (size_t i = 0; i < n + 2; i++) screen_putc('_'); screen_putc('\n');
    screen_puts("< "); screen_puts(text); screen_puts(" >\n");
    screen_putc(' '); for (size_t i = 0; i < n + 2; i++) screen_putc('-'); screen_putc('\n');
    screen_puts("        \\   ^__^\n         \\  (oo)\\_______\n            (__)\\       )\\/\\\n                ||----w |\n                ||     ||\n");
    return 0;
}

/* ---------- colour ---------- */
static const char *const color_names[16] = { "black", "blue", "green", "cyan", "red", "magenta", "brown", "grey", "darkgrey", "lightblue", "lightgreen", "lightcyan", "lightred", "lightmagenta", "yellow", "white" };
static bool parse_color(const char *s, uint8_t *c) {
    uint32_t v;
    for (uint8_t i = 0; i < 16; i++) if (!strcmp(s, color_names[i])) { *c = i; return true; }
    if (cu_parse_count(s, &v, 15)) { *c = (uint8_t)v; return true; }
    return false;
}
static int cmd_color(int argc, const char **argv) {
    uint8_t fg, bg = VGA_BLACK;
    if (argc == 1) {
        for (uint8_t i = 0; i < 16; i++) { screen_set_color(i == 0 ? VGA_WHITE : i, i == 0 ? VGA_BLUE : VGA_BLACK); cu_num_pad(i, 2); screen_putc(' '); screen_puts(color_names[i]); screen_set_color(VGA_LIGHT_GREY, VGA_BLACK); screen_putc(i % 4 == 3 ? '\n' : ' '); screen_puts(i % 4 == 3 ? "" : "  "); }
        screen_puts("usage: color <text> [background]   or   color reset\n");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "reset")) { screen_set_default_color(VGA_LIGHT_GREY, VGA_BLACK); screen_puts("Colours restored.\n"); return 0; }
    if (argc > 3 || !parse_color(argv[1], &fg) || (argc == 3 && !parse_color(argv[2], &bg))) return cu_error("color", "give colour names (black blue green cyan red magenta brown grey darkgrey light... yellow white) or numbers 0-15.");
    if (fg == bg) return cu_error("color", "text and background would be the same colour: unreadable.");
    screen_set_default_color(fg, bg);
    screen_puts("Colours changed (use 'color reset' to go back).\n");
    return 0;
}

/* ---------- time tools ---------- */
static int cmd_timer(int argc, const char **argv) {
    uint32_t s;
    if (argc != 2 || !cu_parse_count(argv[1], &s, 60) || s == 0) return cu_usage("timer <seconds 1-60>     (counts down; the shell is busy until it ends)");
    for (uint32_t i = s; i > 0; i--) { screen_put_dec(i); screen_puts("... "); cu_sleep_ms(1000); }
    screen_puts("time is up!\n");
    return 0;
}
static uint32_t sw_start, sw_frozen; static bool sw_running;
static int cmd_stopwatch(int argc, const char **argv) {
    const char *op = argc == 2 ? argv[1] : "show";
    if (argc > 2) return cu_usage("stopwatch [start|stop|show|reset]");
    uint32_t now = sys_time();
    if (!strcmp(op, "start")) { sw_start = now; sw_running = true; screen_puts("Stopwatch started.\n"); return 0; }
    if (!strcmp(op, "reset")) { sw_running = false; sw_start = 0; sw_frozen = 0; screen_puts("Stopwatch reset.\n"); return 0; }
    if (!strcmp(op, "stop") || !strcmp(op, "show")) {
        if (!sw_running && sw_start == 0) return cu_error("stopwatch", "not started (use: stopwatch start).");
        if (!strcmp(op, "stop") && sw_running) { sw_frozen = now - sw_start; }
        uint32_t t = sw_running && strcmp(op, "stop") ? now - sw_start : (sw_running ? sw_frozen : sw_frozen);
        screen_put_dec(t / TIMER_HZ); screen_putc('.'); screen_put_dec((t % TIMER_HZ) / 10); screen_put_dec(t % 10); screen_puts(" s");
        if (!strcmp(op, "stop") && sw_running) { sw_running = false; screen_puts(" (stopped)"); } else if (!sw_running) screen_puts(" (stopped earlier)"); else screen_puts(" (running)");
        screen_putc('\n');
        return 0;
    }
    return cu_usage("stopwatch [start|stop|show|reset]");
}

/* ---------- demo and credits ---------- */
static shell_exec_fn demo_exec;
void misc_set_executor(shell_exec_fn fn) { demo_exec = fn; }   /* called together with shellenv_set_executor */

static int cmd_demo(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("demo     (a guided tour: press Enter for each step, Ctrl+C to stop)");
    if (!demo_exec) return cu_error("demo", "no shell is available to run the steps.");
    static const struct { const char *title, *cmd; } steps[] = {
        { "Which system is this?", "sysinfo" },
        { "The file system: a directory tree in memory", "ls /" },
        { "Create a directory", "mkdir /tmp/demo" },
        { "Write a file with redirection", "echo Hello MiniOS > /tmp/demo/hello.txt" },
        { "Read it back", "cat /tmp/demo/hello.txt" },
        { "Who owns it, and with what permissions?", "perms /tmp/demo/hello.txt" },
        { "Running processes", "ps" },
        { "Memory usage", "mem" },
        { "Who is logged in?", "whoami" },
        { "Every account", "users" },
        { "Real CPU data through CPUID", "cpuinfo" },
        { "Clean up", "rm /tmp/demo/hello.txt" },
        { "Remove the directory", "rmdir /tmp/demo" },
    };
    cu_color(VGA_LIGHT_CYAN); screen_puts("MiniOS guided demo: press Enter to run each step, Ctrl+C to stop.\n"); cu_color(VGA_LIGHT_GREY);
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        cu_color(VGA_YELLOW); screen_puts("\n["); screen_put_dec((uint32_t)i + 1); screen_putc('/'); screen_put_dec((uint32_t)(sizeof steps / sizeof steps[0])); screen_puts("] "); screen_puts(steps[i].title); cu_color(VGA_LIGHT_GREY);
        screen_puts("\n  $ "); screen_puts(steps[i].cmd); screen_puts("   (Enter)");
        char c;
        do { if (sys_read(0, &c, 1) != 1) c = 3; } while (c != '\n' && c != '\r' && c != 3);
        screen_putc('\n');
        if (c == 3) { screen_puts("Demo stopped.\n"); return 1; }
        demo_exec(steps[i].cmd);
    }
    screen_puts("\nDemo finished.\n");
    return 0;
}
static int cmd_credits(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("credits");
    screen_puts("MiniOS " MINIOS_VERSION " - a Software Engineering / Operating Systems course project.\n");
    screen_puts("Built phase by phase: boot, console, interrupts, shell, commands, memory, processes,\n");
    screen_puts("file system, system calls, users and permissions, and the command library.\n");
    screen_puts("Tools: GCC, NASM, GRUB, QEMU, GNU Make.\n");
    return 0;
}

static const struct { const char *name; cmd_handler_t fn; } bindings[] = {
    { "calc", cmd_calc }, { "cal", cmd_cal }, { "random", cmd_random }, { "base", cmd_base }, { "color", cmd_color }, { "timer", cmd_timer },
    { "stopwatch", cmd_stopwatch }, { "factor", cmd_factor }, { "prime", cmd_prime }, { "fib", cmd_fib }, { "gcd", cmd_gcd }, { "seq", cmd_seq },
    { "yes", cmd_yes }, { "dice", cmd_dice }, { "cowsay", cmd_cowsay }, { "demo", cmd_demo }, { "credits", cmd_credits },
};

int misc_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
