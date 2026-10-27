/* Commands implemented so far, plus the stub handler for the rest.
 * To implement a new command: write cmd_xxx(), add { "xxx", cmd_xxx } to
 * bindings[] below. Nothing else changes - the registry already knows the name,
 * level and description. */
#include "include/builtins.h"
#include "include/shell.h"
#include "include/screen.h"
#include "include/timer.h"
#include "include/version.h"
#include "include/string.h"

/* ---------- small output helpers ---------- */
static void put_pad(const char *s, size_t width) {
    screen_puts(s);
    for (size_t k = strlen(s); k < width; k++) screen_putc(' ');
}
static void put_num_pad(uint32_t v, size_t width) {
    size_t digits = 1;
    for (uint32_t t = v; t >= 10; t /= 10) digits++;
    for (size_t k = digits; k < width; k++) screen_putc(' ');
    screen_put_dec(v);
}
static char lower_c(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static bool contains_ci(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0) return true;
    for (size_t i = 0; hay[i]; i++) {
        size_t k = 0;
        while (k < n && hay[i + k] && lower_c(hay[i + k]) == lower_c(needle[k])) k++;
        if (k == n) return true;
    }
    return false;
}
static void color(uint8_t fg) { screen_set_color(fg, VGA_BLACK); }

/* ---------- stub handler ---------- */
int command_stub(const command_t *c, int argc, const char **argv) {
    (void)argc; (void)argv;
    bool sim = (c->level == LEVEL_L3);
    color(sim ? VGA_LIGHT_MAGENTA : VGA_YELLOW);
    screen_puts(sim ? "[SIMULATED] " : "[STUB] ");
    color(VGA_LIGHT_GREY);
    screen_puts(c->name);
    screen_puts(" (#"); screen_put_dec(c->id);
    screen_puts(", "); screen_puts(command_level_name(c->level));
    screen_puts(", "); screen_puts(c->subsys);
    screen_puts(sim ? "): will be a SIMULATED command; not implemented yet.\n"
                    : "): registered but not implemented yet.\n");
    int phase = command_planned_phase(c);
    screen_puts("       ");
    if (phase > MINIOS_PHASE_NUM) {
        screen_puts("Needs the "); screen_puts(command_planned_phase_what(c));
        screen_puts(" (Phase "); screen_put_dec((uint32_t)phase);
        screen_puts(").\n");
    } else {
        screen_puts("Its prerequisites exist, but nobody has written it yet.\n");
    }
    return 2;
}

/* ---------- registry-aware commands ---------- */
static void print_detail(const command_t *c) {
    screen_puts(c->name); screen_puts("  (#"); screen_put_dec(c->id); screen_puts(")\n");
    screen_puts("  Description : "); screen_puts(c->desc); screen_putc('\n');
    screen_puts("  Category    : "); screen_puts(command_category_name((cmd_category_t)c->category)); screen_putc('\n');
    screen_puts("  Level       : "); screen_puts(command_level_name(c->level));
    screen_puts(" - "); screen_puts(command_level_meaning(c->level)); screen_putc('\n');
    screen_puts("  Subsystem   : "); screen_puts(c->subsys); screen_putc('\n');
    screen_puts("  Status      : ");
    if (c->handler) { color(VGA_LIGHT_GREEN); screen_puts("implemented"); color(VGA_LIGHT_GREY); }
    else { color(VGA_YELLOW); screen_puts("not implemented yet"); color(VGA_LIGHT_GREY);
           screen_puts(" (needs "); screen_puts(command_planned_phase_what(c));
           screen_puts(", Phase "); screen_put_dec((uint32_t)command_planned_phase(c)); screen_putc(')'); }
    screen_putc('\n');
}

static int help_category(cmd_category_t cat) {
    size_t impl;
    size_t total = command_count_category(cat, &impl);
    screen_puts(command_category_name(cat)); screen_puts(": ");
    screen_put_dec((uint32_t)total); screen_puts(" commands, ");
    screen_put_dec((uint32_t)impl); screen_puts(" implemented\n");
    size_t col = 0;
    for (size_t i = 0; i < command_count(); i++) {
        const command_t *c = command_at(i);
        if (c->category != cat) continue;
        color(c->handler ? VGA_WHITE : VGA_DARK_GREY);
        put_pad(c->name, 13);
        if (++col == 6) { screen_putc('\n'); col = 0; }
    }
    if (col) screen_putc('\n');
    color(VGA_LIGHT_GREY);
    screen_puts("white = implemented, grey = registered but not implemented yet\n");
    return 0;
}

static int cmd_help(int argc, const char **argv) {
    if (argc >= 2) {
        int cat = command_category_from_name(argv[1]);
        const command_t *c = command_find(argv[1]);
        if (cat >= 0) {
            help_category((cmd_category_t)cat);
            if (c) { screen_puts("(there is also a command named '"); screen_puts(c->name);
                     screen_puts("': use cmdinfo "); screen_puts(c->name); screen_puts(")\n"); }
            return 0;
        }
        if (c) { print_detail(c); return 0; }
        screen_puts("help: no category or command named '"); screen_puts(argv[1]); screen_puts("'. Try 'help'.\n");
        return 1;
    }
    screen_puts("MiniOS commands: "); screen_put_dec((uint32_t)command_count());
    screen_puts(" registered, "); screen_put_dec((uint32_t)command_count_implemented());
    screen_puts(" implemented\n");
    for (int i = 0; i < CAT_COUNT; i++) {
        size_t impl, total = command_count_category((cmd_category_t)i, &impl);
        screen_puts("  "); put_pad(command_category_name((cmd_category_t)i), 10);
        put_num_pad((uint32_t)total, 3); screen_puts(" commands, ");
        put_num_pad((uint32_t)impl, 2); screen_puts(" implemented\n");
    }
    screen_puts("Use: help <category> | cmdinfo <command> | apropos <word> | cmdcount\n"
                "Keys: Up/Down = history, Left/Right/Home/End/Del = edit, Tab = complete, PgUp/PgDn = scrollback,\n      Ctrl+A/E = start/end of line, Ctrl+U/K/W = cut line/rest/word, Ctrl+C = cancel, Ctrl+L = clear\n");
    return 0;
}

static int cmd_cmdcount(int argc, const char **argv) {
    (void)argc; (void)argv;
    screen_put_dec((uint32_t)command_count()); screen_puts(" commands registered\n");
    screen_puts("  L1 real: ");      screen_put_dec((uint32_t)command_count_level(LEVEL_L1));
    screen_puts("   L2 real: ");     screen_put_dec((uint32_t)command_count_level(LEVEL_L2));
    screen_puts("   L3 simulated: "); screen_put_dec((uint32_t)command_count_level(LEVEL_L3));
    screen_puts("\n  implemented: "); screen_put_dec((uint32_t)command_count_implemented());
    screen_puts("   stubs: ");        screen_put_dec((uint32_t)(command_count() - command_count_implemented()));
    screen_putc('\n');
    return 0;
}

static int info_common(int argc, const char **argv, const char *usage) {
    if (argc < 2) { screen_puts("usage: "); screen_puts(usage); screen_putc('\n'); return 1; }
    const command_t *c = command_find(argv[1]);
    if (!c) { screen_puts(argv[0]); screen_puts(": no command named '"); screen_puts(argv[1]); screen_puts("'\n"); return 1; }
    print_detail(c);
    return 0;
}
static int cmd_cmdinfo(int argc, const char **argv) { return info_common(argc, argv, "cmdinfo <command>"); }
static int cmd_man(int argc, const char **argv)     { return info_common(argc, argv, "man <command>"); }

static int cmd_type(int argc, const char **argv) {
    if (argc < 2) { screen_puts("usage: type <name>\n"); return 1; }
    const command_t *c = command_find(argv[1]);
    if (!c) { screen_puts(argv[1]); screen_puts(": not found\n"); return 1; }
    screen_puts(c->name); screen_puts(" is a shell built-in (#"); screen_put_dec(c->id);
    screen_puts(", "); screen_puts(command_level_name(c->level)); screen_puts(", ");
    screen_puts(c->handler ? "implemented)\n" : "stub)\n");
    return 0;
}

static int cmd_which(int argc, const char **argv) {
    if (argc < 2) { screen_puts("usage: which <command>\n"); return 1; }
    const command_t *c = command_find(argv[1]);
    if (!c) { screen_puts("which: "); screen_puts(argv[1]); screen_puts(" not found\n"); return 1; }
    screen_puts(c->name); screen_puts(": built-in #"); screen_put_dec(c->id); screen_putc('\n');
    return 0;
}

static int cmd_apropos(int argc, const char **argv) {
    if (argc < 2) { screen_puts("usage: apropos <word>\n"); return 1; }
    const uint32_t MAX_SHOWN = 18;
    uint32_t found = 0;
    for (size_t i = 0; i < command_count(); i++) {
        const command_t *c = command_at(i);
        if (!contains_ci(c->name, argv[1]) && !contains_ci(c->desc, argv[1])) continue;
        if (found < MAX_SHOWN) { put_pad(c->name, 12); screen_puts("- "); screen_puts(c->desc); screen_putc('\n'); }
        found++;
    }
    if (found == 0) { screen_puts("apropos: nothing appropriate for '"); screen_puts(argv[1]); screen_puts("'\n"); return 1; }
    if (found > MAX_SHOWN) { screen_put_dec(found - MAX_SHOWN); screen_puts(" more - use a more specific word\n"); }
    return 0;
}

/* ---------- commands carried over from Phase 4 ---------- */
static int cmd_clear(int argc, const char **argv) { (void)argc; (void)argv; screen_clear(); return 0; }

static int cmd_echo(int argc, const char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) screen_putc(' ');
        screen_puts(argv[i]);
    }
    screen_putc('\n');
    return 0;
}

static int cmd_history(int argc, const char **argv) {
    (void)argc; (void)argv;
    for (int i = 0; i < history_count(); i++) {
        screen_put_dec((uint32_t)(i + 1)); screen_puts("  ");
        screen_puts(history_get(i)); screen_putc('\n');
    }
    return 0;
}

static int cmd_version(int argc, const char **argv) {
    (void)argc; (void)argv;
    screen_puts("MiniOS v" MINIOS_VERSION " (" MINIOS_PHASE ") - educational x86 OS\n");
    return 0;
}

static int cmd_uptime(int argc, const char **argv) {
    (void)argc; (void)argv;
    uint32_t t = timer_ticks();
    screen_puts("up "); screen_put_dec(t / TIMER_HZ); screen_putc('.');
    screen_put_dec((t % TIMER_HZ) * 10 / TIMER_HZ);
    screen_puts(" s  ("); screen_put_dec(t); screen_puts(" ticks @ ");
    screen_put_dec(TIMER_HZ); screen_puts(" Hz)\n");
    return 0;
}

static int cmd_sleep(int argc, const char **argv) {
    uint32_t ms = 1000;
    if (argc > 1) {
        ms = 0;
        for (const char *p = argv[1]; *p; p++) {
            if (*p < '0' || *p > '9') { screen_puts("Error: sleep needs milliseconds, e.g. sleep 500\n"); return 1; }
            ms = ms * 10 + (uint32_t)(*p - '0');
            if (ms > 60000) { screen_puts("Error: maximum is 60000 ms\n"); return 1; }
        }
    }
    timer_sleep_ms(ms);
    return 0;
}

/* legacy Phase 4 names: "int3" runs breakpoint, "div0" runs panic (see shell.c) */
static int cmd_breakpoint(int argc, const char **argv) { (void)argc; (void)argv; __asm__ volatile ("int3"); return 0; }

static int cmd_panic(int argc, const char **argv) {
    (void)argc; (void)argv;
    screen_puts("Triggering a test kernel panic (divide by zero)...\n");
    volatile int z = 0; volatile int r = 1 / z; (void)r;
    return 0;
}

static int cmd_true(int argc, const char **argv)  { (void)argc; (void)argv; return 0; }
static int cmd_false(int argc, const char **argv) { (void)argc; (void)argv; return 1; }

static int cmd_exitcode(int argc, const char **argv) {
    (void)argc; (void)argv;
    screen_puts("exit status of the previous command: "); screen_put_dec((uint32_t)shell_last_exit()); screen_putc('\n');
    return 0;
}

/* ---------- binding table: the single place that says "this command is real" ---------- */
static const struct { const char *name; cmd_handler_t fn; } bindings[] = {
    { "help",       cmd_help       },
    { "clear",      cmd_clear      },
    { "echo",       cmd_echo       },
    { "history",    cmd_history    },
    { "sleep",      cmd_sleep      },
    { "true",       cmd_true       },
    { "false",      cmd_false      },
    { "type",       cmd_type       },
    { "which",      cmd_which      },
    { "man",        cmd_man        },
    { "apropos",    cmd_apropos    },
    { "cmdcount",   cmd_cmdcount   },
    { "cmdinfo",    cmd_cmdinfo    },
    { "version",    cmd_version    },
    { "uptime",     cmd_uptime     },
    { "exitcode",   cmd_exitcode   },
    { "breakpoint", cmd_breakpoint },
    { "panic",      cmd_panic      },
};

int builtins_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    failed += memory_commands_register();     /* Phase 6 */
    failed += process_commands_register();    /* Phase 7 */
    failed += directory_commands_register();  /* Phase 8 */
    failed += file_commands_register();
    failed += text_commands_register();
    failed += fsinfo_commands_register();
    failed += syscall_commands_register();    /* Phase 9 */
    failed += user_commands_register();       /* Phase 10 */
    failed += shellenv_commands_register();   /* Phase 11 */
    failed += textops_commands_register();
    failed += sysinfo_commands_register();
    failed += disk_commands_register();
    failed += misc_commands_register();
    failed += network_commands_register();
    failed += diag_commands_register();
    failed += fileextra_commands_register();
    failed += log_commands_register();
    failed += jobs_commands_register();
    failed += blkextra_commands_register();
    return failed;
}
