/* Process commands (Phase 7; moved onto system calls in Phase 9): ps spawn kill wait top pstree pid
 * ppid nice renice priority suspend resume sched quantum setquantum yield jobs pinfo pkill killall
 * pgrep runqueue ctxstat proccount procdemo.
 *
 * Everything that manages processes goes through sys_process_* / sys_getpid / sys_yield. The four
 * scheduler *views* (sched, quantum/setquantum, runqueue, ctxstat) still read scheduler internals
 * directly: the ready queue and time slice are not exposed by the system-call interface yet. */
#include "include/builtins.h"
#include "include/screen.h"
#include "include/keyboard.h"
#include "include/timer.h"
#include "include/process.h"
#include "include/scheduler.h"
#include "include/workloads.h"
#include "include/syscall.h"
#include "include/numparse.h"
#include "include/string.h"

typedef sys_procinfo_t pinfo_t;

/* ---------- helpers ---------- */
static void color(uint8_t fg) { screen_set_color(fg, VGA_BLACK); }
static void put_pad(const char *s, size_t w) { screen_puts(s); for (size_t k = strlen(s); k < w; k++) screen_putc(' '); }
static void put_num_pad(uint32_t v, size_t w) {
    char t[12]; size_t n = 0; uint32_t x = v;
    do { t[n++] = (char)('0' + x % 10); x /= 10; } while (x);
    for (size_t k = n; k < w; k++) screen_putc(' ');
    while (n) screen_putc(t[--n]);
}
static void put_int(int v) { if (v < 0) { screen_putc('-'); screen_put_dec((uint32_t)(-v)); } else screen_put_dec((uint32_t)v); }
static int usage(const char *text) { screen_puts("usage: "); screen_puts(text); screen_putc('\n'); return 1; }
static int fail(const char *cmd, const char *msg) {
    color(VGA_LIGHT_RED); screen_puts("Error: "); color(VGA_LIGHT_GREY);
    screen_puts(cmd); screen_puts(": "); screen_puts(msg); screen_putc('\n');
    return 1;
}

static bool parse_pid(const char *s, int *pid) {
    uint32_t v;
    if (!parse_u32(s, &v) || v > 0x7FFFFFFFu) return false;
    *pid = (int)v; return true;
}

/* Prints the standard message for a system-call error code. */
static int report(const char *cmd, int pid, int rc) {
    pinfo_t info; bool known = sys_process_info(pid, &info) == 0;
    color(VGA_LIGHT_RED); screen_puts("Error: "); color(VGA_LIGHT_GREY);
    switch (rc) {
        case SYS_ESRCH: screen_puts("process "); put_int(pid); screen_puts(" does not exist."); break;
        case SYS_EPERM: screen_puts("process "); put_int(pid);
                        if (known) { screen_puts(" ("); screen_puts(info.name); screen_putc(')'); }
                        screen_puts(" is protected: "); screen_puts(cmd); screen_puts(" refused."); break;
        case SYS_ESTATE: screen_puts("process "); put_int(pid);
                        screen_puts(known && info.state == PROC_TERMINATED ? " is already terminated." : " is not in a state where this applies.");
                        if (known) { screen_puts(" (state "); screen_puts(process_state_name(info.state)); screen_putc(')'); } break;
        default: screen_puts(cmd); screen_puts(": "); screen_puts(sys_strerror(rc)); break;
    }
    screen_putc('\n');
    return 1;
}

static const char *wait_text(const pinfo_t *p) {
    if (p->state != PROC_WAITING) return "";
    switch (p->wait_kind) { case WAIT_SLEEP: return "sleeping"; case WAIT_CHILD: return "waiting for child"; case WAIT_SUSPENDED: return "suspended"; default: return "waiting"; }
}

/* ---------- process table through sys_process_list / sys_process_info ---------- */
static size_t collect(pinfo_t *out, bool include_idle, int only_parent) {
    int pids[PROC_MAX];
    int n = sys_process_list(pids, PROC_MAX);
    size_t k = 0;
    for (int i = 0; i < n && k < PROC_MAX; i++) {
        pinfo_t info;
        if (sys_process_info(pids[i], &info) != 0) continue;
        if (!include_idle && info.pid == PID_IDLE) continue;
        if (only_parent >= 0 && info.parent_pid != only_parent) continue;
        out[k++] = info;                                    /* the list is already sorted by pid */
    }
    return k;
}

static void print_table(bool include_idle, int only_parent) {
    pinfo_t list[PROC_MAX];
    size_t n = collect(list, include_idle, only_parent);
    screen_puts("PID   NAME          STATE       PRIORITY  CPU     PPID  MEM    EXIT\n");
    for (size_t i = 0; i < n; i++) {
        const pinfo_t *p = &list[i];
        uint8_t c = p->state == PROC_RUNNING ? VGA_LIGHT_GREEN : p->state == PROC_TERMINATED ? VGA_DARK_GREY :
                    p->state == PROC_WAITING ? VGA_YELLOW : VGA_LIGHT_GREY;
        color(c);
        put_num_pad((uint32_t)p->pid, 3); screen_puts("   ");
        put_pad(p->name, 14); put_pad(process_state_name(p->state), 12);
        put_num_pad((uint32_t)p->priority, 8); screen_puts("  ");
        put_num_pad(p->cpu_ticks, 6); screen_puts("  ");
        put_num_pad((uint32_t)p->parent_pid, 4); screen_puts("  ");
        put_num_pad((uint32_t)p->memory_used >> 10, 4); screen_puts("K  ");
        if (p->state == PROC_TERMINATED) put_int(p->exit_code); else screen_putc('-');
        color(VGA_LIGHT_GREY);
        if (p->state == PROC_WAITING) { screen_puts("  ("); screen_puts(wait_text(p)); screen_putc(')'); }
        screen_putc('\n');
    }
}

static int cmd_ps(int argc, const char **argv) {
    bool all = argc >= 2 && !strcmp(argv[1], "-a");
    if (argc >= 2 && !all) return usage("ps [-a]   (-a also shows the idle task)");
    print_table(all, -1);
    return 0;
}

static int cmd_jobs(int argc, const char **argv) {
    (void)argc; (void)argv;
    pinfo_t list[PROC_MAX];
    size_t n = collect(list, false, PID_SHELL);
    if (n == 0) { screen_puts("no jobs\n"); return 0; }
    for (size_t i = 0; i < n; i++) {
        screen_putc('['); screen_put_dec((uint32_t)list[i].pid); screen_puts("]  ");
        put_pad(process_state_name(list[i].state), 12); screen_puts(list[i].name); screen_putc('\n');
    }
    return 0;
}

static int cmd_top(int argc, const char **argv) {
    uint32_t refreshes = 0;                       /* 0 = until a key is pressed */
    if (argc >= 2 && (!parse_u32(argv[1], &refreshes) || refreshes == 0 || refreshes > 3600)) return fail("top", "refresh count must be 1-3600");
    for (uint32_t r = 0; refreshes == 0 || r < refreshes; r++) {
        sys_info_t si; sys_sysinfo(&si);
        screen_clear();
        screen_puts("MiniOS top - up "); screen_put_dec(si.uptime_ticks / si.ticks_per_sec); screen_puts("s, ");
        screen_put_dec(si.processes); screen_puts(" processes, time slice ");
        screen_put_dec(sched_timeslice()); screen_puts(" ticks, ");
        screen_put_dec(si.context_switches); screen_puts(" context switches\n\n");
        print_table(true, -1);
        screen_puts("\nPress any key to quit.\n");
        for (int i = 0; i < 10; i++) {            /* 1 second, polling the keyboard */
            if (keyboard_has_key()) { (void)keyboard_getkey(); screen_clear(); return 0; }
            sys_sleep(100);
        }
    }
    return 0;
}

/* ---------- spawn / nice / procdemo ---------- */
static void list_workloads(void) {
    screen_puts("usage: spawn <kind> [arg]\nkinds:\n");
    for (size_t i = 0; i < workload_count(); i++) {
        const workload_t *w = workload_at(i);
        screen_puts("  "); put_pad(w->kind, 9); put_pad(w->arg_help ? w->arg_help : "", 17); screen_puts(w->help); screen_putc('\n');
    }
}

static int do_spawn(const char *cmd, const char *kind, const char *arg, int priority) {
    const workload_t *w = workload_find(kind);
    if (!w) {
        color(VGA_LIGHT_RED); screen_puts("Error: "); color(VGA_LIGHT_GREY);
        screen_puts(cmd); screen_puts(": unknown program '"); screen_puts(kind); screen_puts("'. Run 'spawn' to list them.\n");
        return 1;
    }
    uint32_t value = w->default_arg;
    if (arg) {
        if (!w->arg_help) return fail(cmd, "this program takes no argument");
        if (!parse_u32(arg, &value) || value < 1 || value > 600) return fail(cmd, "argument must be a number from 1 to 600");
    }
    int pid = sys_process_create(w->kind, value, priority);
    if (pid < 0) return fail(cmd, sys_strerror(pid));
    screen_puts("spawned process "); screen_put_dec((uint32_t)pid); screen_puts(" '"); screen_puts(w->kind);
    screen_puts("' (priority "); screen_put_dec((uint32_t)priority); screen_puts(")\n");
    return 0;
}

static int cmd_spawn(int argc, const char **argv) {
    if (argc < 2) { list_workloads(); return 0; }
    if (argc > 3) return usage("spawn <kind> [arg]");
    return do_spawn("spawn", argv[1], argc == 3 ? argv[2] : NULL, PRIORITY_DEFAULT);
}

static int cmd_nice(int argc, const char **argv) {
    if (argc < 3 || argc > 4) return usage("nice <priority 1-20> <kind> [arg]");
    uint32_t prio;
    if (!parse_u32(argv[1], &prio) || prio < PRIORITY_MIN || prio > PRIORITY_MAX) return fail("nice", "priority must be 1-20");
    return do_spawn("nice", argv[2], argc == 4 ? argv[3] : NULL, (int)prio);
}

static int cmd_procdemo(int argc, const char **argv) {
    (void)argc; (void)argv;
    static const char *names[3] = { "demo-a", "demo-b", "demo-c" };
    int made = 0;
    for (int i = 0; i < 3; i++) {
        int pid = sys_process_create_named("demo", names[i], 3, PRIORITY_DEFAULT);
        if (pid < 0) { fail("procdemo", sys_strerror(pid)); break; }
        made++;
    }
    if (made == 0) return 1;
    screen_puts("started "); screen_put_dec((uint32_t)made); screen_puts(" CPU-bound processes (3 s of CPU each). Run 'ps' a few times:\n"
                "round robin gives each the same number of CPU ticks while they compete.\n");
    return made == 3 ? 0 : 1;
}

/* ---------- kill family ---------- */
static int cmd_kill(int argc, const char **argv) {
    if (argc != 2) return usage("kill <pid>");
    int pid;
    if (!parse_pid(argv[1], &pid)) return fail("kill", "not a valid process id");
    pinfo_t info; char name[PROC_NAME_MAX]; name[0] = '\0';
    if (sys_process_info(pid, &info) == 0) memcpy(name, info.name, sizeof name);
    int rc = sys_process_kill(pid);
    if (rc != 0) return report("kill", pid, rc);
    screen_puts("killed process "); screen_put_dec((uint32_t)pid); screen_puts(" ("); screen_puts(name); screen_puts(")\n");
    return 0;
}

static int kill_by_name(const char *cmd, const char *name, bool all) {
    int killed = 0;
    for (int round = 0; round < PROC_MAX; round++) {
        pinfo_t list[PROC_MAX];
        size_t n = collect(list, true, -1);
        int target = -1;
        for (size_t i = 0; i < n; i++) {
            const pinfo_t *p = &list[i];
            if (p->pid > PID_SHELL && p->state != PROC_TERMINATED && !strcmp(p->name, name)) { target = p->pid; break; }   /* lowest pid first */
        }
        if (target < 0) break;
        if (sys_process_kill(target) != 0) break;
        screen_puts("killed process "); screen_put_dec((uint32_t)target); screen_puts(" ("); screen_puts(name); screen_puts(")\n");
        killed++;
        if (!all) break;
    }
    if (!killed) { color(VGA_LIGHT_RED); screen_puts("Error: "); color(VGA_LIGHT_GREY); screen_puts(cmd); screen_puts(": no running process named '"); screen_puts(name); screen_puts("'\n"); return 1; }
    return 0;
}
static int cmd_pkill(int argc, const char **argv)   { return argc == 2 ? kill_by_name("pkill", argv[1], false) : usage("pkill <name>   (kills the lowest PID with that name)"); }
static int cmd_killall(int argc, const char **argv) { return argc == 2 ? kill_by_name("killall", argv[1], true) : usage("killall <name>"); }

static int cmd_pgrep(int argc, const char **argv) {
    if (argc != 2) return usage("pgrep <name>");
    pinfo_t list[PROC_MAX]; size_t n = collect(list, true, -1); int found = 0;
    for (size_t i = 0; i < n; i++) if (!strcmp(list[i].name, argv[1]) && list[i].state != PROC_TERMINATED) { screen_put_dec((uint32_t)list[i].pid); screen_putc('\n'); found++; }
    return found ? 0 : 1;
}

/* ---------- wait / suspend / resume / yield ---------- */
static int cmd_wait(int argc, const char **argv) {
    int pid = -1;
    if (argc > 2) return usage("wait [pid]");
    if (argc == 2 && !parse_pid(argv[1], &pid)) return fail("wait", "not a valid process id");
    if (pid == sys_getpid()) return fail("wait", "a process cannot wait for itself");
    pinfo_t t;
    if (pid >= 0 && sys_process_info(pid, &t) == 0 && t.state != PROC_TERMINATED && t.parent_pid == sys_getpid()) {
        screen_puts("waiting for process "); screen_put_dec((uint32_t)pid); screen_puts(" (Ctrl+C stops waiting)...\n");
    }
    int code = 0, rc = sys_process_wait(pid, &code);
    if (rc == SYS_EINTR) { screen_puts("wait interrupted; the process keeps running\n"); return 130; }
    if (rc == SYS_ECHILD) {
        color(VGA_LIGHT_RED); screen_puts("Error: "); color(VGA_LIGHT_GREY);
        if (pid < 0) screen_puts("wait: this shell has no child processes\n");
        else { screen_puts("wait: process "); put_int(pid); screen_puts(" is not a child of the shell\n"); }
        return 1;
    }
    if (rc < 0) return report("wait", pid, rc);
    screen_puts("process "); screen_put_dec((uint32_t)rc); screen_puts(" finished with exit code "); put_int(code);
    if (code == PROC_EXIT_KILLED) screen_puts(" (killed)");
    else if (code == PROC_EXIT_STACK_OVERFLOW) screen_puts(" (stack overflow)");
    else if (code >= PROC_EXIT_FAULT_BASE && code < PROC_EXIT_FAULT_BASE + 32) screen_puts(" (CPU exception)");
    screen_putc('\n');
    return 0;
}

static int cmd_suspend(int argc, const char **argv) {
    int pid; if (argc != 2) return usage("suspend <pid>");
    if (!parse_pid(argv[1], &pid)) return fail("suspend", "not a valid process id");
    int rc = sys_process_suspend(pid);
    if (rc != 0) return report("suspend", pid, rc);
    screen_puts("process "); screen_put_dec((uint32_t)pid); screen_puts(" suspended (WAITING)\n");
    return 0;
}
static int cmd_resume(int argc, const char **argv) {
    int pid; if (argc != 2) return usage("resume <pid>");
    if (!parse_pid(argv[1], &pid)) return fail("resume", "not a valid process id");
    int rc = sys_process_resume(pid);
    if (rc == SYS_ESTATE) return fail("resume", "process is not suspended");
    if (rc != 0) return report("resume", pid, rc);
    screen_puts("process "); screen_put_dec((uint32_t)pid); screen_puts(" resumed (READY)\n");
    return 0;
}
static int cmd_yield(int argc, const char **argv) { (void)argc; (void)argv; sys_yield(); return 0; }

/* ---------- identity / priority ---------- */
static int cmd_pid(int argc, const char **argv)  { (void)argc; (void)argv; screen_put_dec((uint32_t)sys_getpid()); screen_putc('\n'); return 0; }
static int cmd_ppid(int argc, const char **argv) { (void)argc; (void)argv; screen_put_dec((uint32_t)sys_getppid()); screen_putc('\n'); return 0; }

static int cmd_priority(int argc, const char **argv) {
    int pid = sys_getpid();
    if (argc > 2) return usage("priority [pid]");
    if (argc == 2 && !parse_pid(argv[1], &pid)) return fail("priority", "not a valid process id");
    pinfo_t p; int rc = sys_process_info(pid, &p);
    if (rc != 0) return report("priority", pid, rc);
    screen_puts("process "); screen_put_dec((uint32_t)pid); screen_puts(" ("); screen_puts(p.name); screen_puts(") priority "); screen_put_dec((uint32_t)p.priority);
    screen_puts("  [recorded only: round robin does not use it]\n");
    return 0;
}

static int cmd_renice(int argc, const char **argv) {
    int pid; uint32_t prio;
    if (argc != 3) return usage("renice <pid> <priority 1-20>");
    if (!parse_pid(argv[1], &pid)) return fail("renice", "not a valid process id");
    if (!parse_u32(argv[2], &prio) || prio < PRIORITY_MIN || prio > PRIORITY_MAX) return fail("renice", "priority must be 1-20");
    int rc = sys_setpriority(pid, (int)prio);
    if (rc != 0) return report("renice", pid, rc);
    screen_puts("process "); screen_put_dec((uint32_t)pid); screen_puts(" priority is now "); screen_put_dec(prio); screen_putc('\n');
    return 0;
}

/* ---------- scheduler views (read scheduler internals directly) ---------- */
static int cmd_sched(int argc, const char **argv) {
    (void)argc; (void)argv;
    sched_stats_t st; sched_stats(&st);
    pinfo_t me; sys_process_info(sys_getpid(), &me);
    screen_puts("Scheduler\n  Policy       : round robin (FIFO ready queue); priority is recorded but not used\n");
    screen_puts("  Time slice   : "); screen_put_dec(st.timeslice); screen_puts(" ticks = "); screen_put_dec(st.timeslice * 1000 / TIMER_HZ);
    screen_puts(" ms  (timer "); screen_put_dec(TIMER_HZ); screen_puts(" Hz)\n");
    screen_puts("  Running      : pid "); screen_put_dec((uint32_t)me.pid); screen_puts(" ("); screen_puts(me.name); screen_puts(")\n");
    screen_puts("  Ready queue  : "); screen_put_dec((uint32_t)sched_ready_count()); screen_puts(" process(es)\n");
    screen_puts("  Context      : real switches (saved registers + separate stacks)\n");
    return 0;
}
static int cmd_quantum(int argc, const char **argv) {
    (void)argc; (void)argv;
    uint32_t t = sched_timeslice();
    screen_puts("time slice: "); screen_put_dec(t); screen_puts(" ticks ("); screen_put_dec(t * 1000 / TIMER_HZ); screen_puts(" ms)\n");
    return 0;
}
static int cmd_setquantum(int argc, const char **argv) {
    uint32_t t;
    if (argc != 2) return usage("setquantum <ticks 1-100>");
    if (!parse_u32(argv[1], &t) || sched_set_timeslice(t) != 0) return fail("setquantum", "time slice must be 1-100 ticks (10 ms each)");
    screen_puts("time slice set to "); screen_put_dec(t); screen_puts(" ticks ("); screen_put_dec(t * 1000 / TIMER_HZ); screen_puts(" ms)\n");
    return 0;
}
static int cmd_runqueue(int argc, const char **argv) {
    (void)argc; (void)argv;
    pinfo_t me; sys_process_info(sys_getpid(), &me);
    screen_puts("running: pid "); screen_put_dec((uint32_t)me.pid); screen_putc(' '); screen_puts(me.name); screen_putc('\n');
    if (sched_ready_count() == 0) { screen_puts("ready queue: (empty)\n"); return 0; }
    screen_puts("ready queue (next first):\n");
    for (size_t i = 0; i < sched_ready_count(); i++) {
        const PCB *p = sched_ready_at(i);
        screen_puts("  "); screen_put_dec((uint32_t)(i + 1)); screen_puts(". pid "); screen_put_dec((uint32_t)p->pid); screen_putc(' '); screen_puts(p->name); screen_putc('\n');
    }
    return 0;
}
static int cmd_ctxstat(int argc, const char **argv) {
    (void)argc; (void)argv;
    sched_stats_t st; sched_stats(&st);
    screen_puts("Context-switch counters\n");
    screen_puts("  switches total : "); screen_put_dec(st.context_switches); screen_putc('\n');
    screen_puts("  preemptions    : "); screen_put_dec(st.preemptions); screen_puts("  (time slice expired)\n");
    screen_puts("  voluntary      : "); screen_put_dec(st.voluntary); screen_puts("  (yield, sleep, wait, exit)\n");
    screen_puts("  timer ticks    : "); screen_put_dec(st.total_ticks); screen_puts("  (idle: "); screen_put_dec(st.idle_ticks); screen_puts(")\n");
    screen_puts("  stack overflows caught: "); screen_put_dec(st.stack_overflows); screen_putc('\n');
    return 0;
}
static int cmd_proccount(int argc, const char **argv) {
    (void)argc; (void)argv;
    static const proc_state_t states[] = { PROC_READY, PROC_RUNNING, PROC_WAITING, PROC_TERMINATED };
    pinfo_t list[PROC_MAX]; size_t n = collect(list, true, -1);
    int total = 0;
    for (size_t s = 0; s < 4; s++) {
        int c = 0; for (size_t i = 0; i < n; i++) if (list[i].state == (int)states[s]) c++;
        total += c; put_pad(process_state_name(states[s]), 12); screen_put_dec((uint32_t)c); screen_putc('\n');
    }
    screen_puts("total       "); screen_put_dec((uint32_t)total); screen_puts("  (table size "); screen_put_dec(PROC_MAX); screen_puts(")\n");
    return 0;
}

static int cmd_pinfo(int argc, const char **argv) {
    int pid = sys_getpid();
    if (argc > 2) return usage("pinfo [pid]");
    if (argc == 2 && !parse_pid(argv[1], &pid)) return fail("pinfo", "not a valid process id");
    pinfo_t p; int rc = sys_process_info(pid, &p);
    if (rc != 0) return report("pinfo", pid, rc);
    screen_puts("Process "); screen_put_dec((uint32_t)p.pid); screen_puts(" '"); screen_puts(p.name); screen_puts("'\n");
    screen_puts("  State        : "); screen_puts(process_state_name(p.state));
    if (p.state == PROC_WAITING) { screen_puts(" ("); screen_puts(wait_text(&p)); screen_putc(')'); }
    screen_putc('\n');
    screen_puts("  Priority     : "); screen_put_dec((uint32_t)p.priority); screen_putc('\n');
    screen_puts("  Parent       : "); screen_put_dec((uint32_t)p.parent_pid); screen_putc('\n');
    screen_puts("  Memory       : "); screen_put_dec((uint32_t)p.memory_used); screen_puts(" bytes (kernel stack)\n");
    screen_puts("  CPU ticks    : "); screen_put_dec(p.cpu_ticks); screen_puts("   scheduled "); screen_put_dec(p.times_scheduled); screen_puts(" times\n");
    screen_puts("  Started at   : tick "); screen_put_dec(p.start_tick); screen_putc('\n');
    if (p.state == PROC_TERMINATED) { screen_puts("  Exit code    : "); put_int(p.exit_code); screen_putc('\n'); }
    return 0;
}

static void tree(const pinfo_t *all, size_t n, int pid, int depth, int *budget) {
    const pinfo_t *p = NULL;
    for (size_t i = 0; i < n; i++) if (all[i].pid == pid) p = &all[i];
    if (!p || depth > 6 || (*budget)-- <= 0) return;
    for (int i = 0; i < depth; i++) screen_puts("  ");
    if (depth) screen_puts("`- ");
    screen_puts(p->name); screen_putc('('); screen_put_dec((uint32_t)p->pid); screen_puts(") "); screen_puts(process_state_name(p->state)); screen_putc('\n');
    for (size_t i = 0; i < n; i++) if (all[i].parent_pid == pid && all[i].pid != pid) tree(all, n, all[i].pid, depth + 1, budget);
}
static int cmd_pstree(int argc, const char **argv) {
    (void)argc; (void)argv;
    pinfo_t all[PROC_MAX]; size_t n = collect(all, true, -1);
    int budget = PROC_MAX; tree(all, n, PID_IDLE, 0, &budget);
    return 0;
}

/* ---------- registration ---------- */
static const struct { const char *name; cmd_handler_t fn; } bindings[] = {
    { "ps",         cmd_ps         },
    { "spawn",      cmd_spawn      },
    { "kill",       cmd_kill       },
    { "wait",       cmd_wait       },
    { "top",        cmd_top        },
    { "pstree",     cmd_pstree     },
    { "pid",        cmd_pid        },
    { "ppid",       cmd_ppid       },
    { "nice",       cmd_nice       },
    { "renice",     cmd_renice     },
    { "priority",   cmd_priority   },
    { "suspend",    cmd_suspend    },
    { "resume",     cmd_resume     },
    { "sched",      cmd_sched      },
    { "quantum",    cmd_quantum    },
    { "setquantum", cmd_setquantum },
    { "yield",      cmd_yield      },
    { "jobs",       cmd_jobs       },
    { "pinfo",      cmd_pinfo      },
    { "pkill",      cmd_pkill      },
    { "killall",    cmd_killall    },
    { "pgrep",      cmd_pgrep      },
    { "runqueue",   cmd_runqueue   },
    { "ctxstat",    cmd_ctxstat    },
    { "proccount",  cmd_proccount  },
    { "procdemo",   cmd_procdemo   },
};

int process_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
