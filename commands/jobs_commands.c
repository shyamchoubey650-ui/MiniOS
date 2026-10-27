/* Job control and per-process memory accounting (Phase 12): bg, fg, memlimit.
 *
 * MiniOS has no terminal job control: there is no Ctrl+Z, and a process keeps running whether or not the shell waits for it.
 *   bg  = resume a suspended child and leave it running in the background.
 *   fg  = resume it if needed and make the shell WAIT for it (Ctrl+C stops waiting; the process keeps running).
 * memlimit is ACCOUNTING ONLY: it records a limit per process and reports which processes are over it. Nothing is stopped. */
#include "include/builtins.h"
#include "include/cmdutil.h"
#include "include/screen.h"
#include "include/syscall.h"
#include "include/process.h"
#include "include/numparse.h"
#include "include/string.h"

static bool parse_pid_arg(const char *s, int *pid) { uint32_t v; if (!parse_u32(s, &v) || v >= PROC_MAX) return false; *pid = (int)v; return true; }

static bool is_suspended(const sys_procinfo_t *t) { return t->state == PROC_WAITING && t->wait_kind == WAIT_SUSPENDED; }

/* Newest live child of the shell; only suspended ones if `suspended_only`. Returns -1 if there is none. */
static int newest_child(bool suspended_only) {
    int pids[PROC_MAX]; int n = sys_process_list(pids, PROC_MAX), best = -1, me = sys_getpid();
    for (int i = 0; i < n; i++) {
        sys_procinfo_t t;
        if (sys_process_info(pids[i], &t) != 0 || t.parent_pid != me || t.state == PROC_TERMINATED) continue;
        if (suspended_only && !is_suspended(&t)) continue;
        if (pids[i] > best) best = pids[i];
    }
    return best;
}

static int not_child(const char *cmd, int pid) { screen_puts("Error: "); screen_puts(cmd); screen_puts(": process "); screen_put_dec((uint32_t)pid); screen_puts(" is not a job of this shell\n"); return 1; }

static int cmd_bg(int argc, const char **argv) {
    int pid;
    if (argc > 2) return cu_usage("bg [pid]      (resumes a suspended job and leaves it running; without a pid: the newest suspended job)");
    if (argc == 2) { if (!parse_pid_arg(argv[1], &pid)) return cu_error("bg", "not a valid process id"); }
    else { pid = newest_child(true); if (pid < 0) return cu_error("bg", "there is no suspended job"); }
    sys_procinfo_t t;
    if (sys_process_info(pid, &t) != 0) return cu_error("bg", "no such process");
    if (t.parent_pid != sys_getpid()) return not_child("bg", pid);
    if (t.state == PROC_TERMINATED) return cu_error("bg", "that job has already finished");
    if (!is_suspended(&t)) { screen_puts("["); screen_put_dec((uint32_t)pid); screen_puts("] "); screen_puts(t.name); screen_puts(" is already running in the background\n"); return 0; }
    int rc = sys_process_resume(pid);
    if (rc != 0) return cu_error("bg", sys_strerror(rc));
    screen_puts("["); screen_put_dec((uint32_t)pid); screen_puts("] "); screen_puts(t.name); screen_puts(" resumed in the background\n");
    return 0;
}

static int cmd_fg(int argc, const char **argv) {
    int pid;
    if (argc > 2) return cu_usage("fg [pid]      (the shell waits for the job; Ctrl+C stops waiting. Without a pid: the newest job)");
    if (argc == 2) { if (!parse_pid_arg(argv[1], &pid)) return cu_error("fg", "not a valid process id"); }
    else { pid = newest_child(false); if (pid < 0) return cu_error("fg", "there is no job"); }
    sys_procinfo_t t;
    if (sys_process_info(pid, &t) != 0) return cu_error("fg", "no such process");
    if (t.parent_pid != sys_getpid()) return not_child("fg", pid);
    if (is_suspended(&t)) {
        int rc = sys_process_resume(pid);
        if (rc != 0) return cu_error("fg", sys_strerror(rc));
        screen_puts("["); screen_put_dec((uint32_t)pid); screen_puts("] "); screen_puts(t.name); screen_puts(" resumed\n");
    }
    if (t.state != PROC_TERMINATED) { screen_puts("waiting for "); screen_puts(t.name); screen_puts(" (Ctrl+C stops waiting; the job keeps running)...\n"); }
    int code = 0, rc = sys_process_wait(pid, &code);
    if (rc == SYS_EINTR) { screen_puts("stopped waiting; the job keeps running in the background\n"); return 130; }
    if (rc < 0) return cu_error("fg", sys_strerror(rc));
    screen_puts("["); screen_put_dec((uint32_t)pid); screen_puts("] "); screen_puts(t.name); screen_puts(" finished with exit code ");
    if (code < 0) { screen_putc('-'); screen_put_dec((uint32_t)(-code)); } else screen_put_dec((uint32_t)code);
    if (code == PROC_EXIT_KILLED) screen_puts(" (killed)"); else if (code == PROC_EXIT_STACK_OVERFLOW) screen_puts(" (stack overflow)");
    else if (code >= PROC_EXIT_FAULT_BASE && code < PROC_EXIT_FAULT_BASE + 32) screen_puts(" (CPU exception)");
    screen_putc('\n');
    return 0;
}

/* ---------- memlimit ---------- */
static struct { bool set; uint32_t start_tick; uint32_t limit; } limits[PROC_MAX];     /* start_tick tells a new process from the old one that had the same pid */

static bool parse_bytes(const char *s, uint32_t *out) {
    size_t n = strlen(s); uint32_t mult = 1;
    if (n == 0) return false;
    char last = s[n - 1];
    char num[16];
    if (last == 'k' || last == 'K') mult = 1024; else if (last == 'm' || last == 'M') mult = 1024 * 1024;
    if (mult != 1) n--;
    if (n == 0 || n >= sizeof num) return false;
    memcpy(num, s, n); num[n] = '\0';
    uint32_t v; if (!parse_u32(num, &v)) return false;
    if (v > 0xFFFFFFFFu / mult) return false;
    *out = v * mult;
    return true;
}

static void put_limit_row(const sys_procinfo_t *t) {
    cu_num_pad((uint32_t)t->pid, 4); screen_puts("  "); cu_pad(t->name, 14); cu_num_pad((uint32_t)t->memory_used, 8); screen_puts("  ");
    if (limits[t->pid].set && limits[t->pid].start_tick == t->start_tick) {
        cu_num_pad(limits[t->pid].limit, 8);
        screen_puts((uint32_t)t->memory_used > limits[t->pid].limit ? "   OVER (not enforced)" : "   ok");
    } else screen_puts("    none");
    screen_putc('\n');
}

static int cmd_memlimit(int argc, const char **argv) {
    if (argc == 1) {
        int pids[PROC_MAX]; int n = sys_process_list(pids, PROC_MAX);
        screen_puts(" pid  name          memory(B)     limit(B)   status\n");
        for (int i = 0; i < n; i++) { sys_procinfo_t t; if (sys_process_info(pids[i], &t) == 0 && t.state != PROC_TERMINATED) put_limit_row(&t); }
        screen_puts("Accounting only: a limit is reported when exceeded, but nothing stops the process.\n");
        return 0;
    }
    int pid;
    if (argc != 3 || !parse_pid_arg(argv[1], &pid)) return cu_usage("memlimit   |   memlimit <pid> <bytes | 64K | 1M | off>      (root; accounting only)");
    cred_info_t c;
    if (sys_getcred(&c) != 0 || c.euid != 0) return cu_error("memlimit", "permission denied: only root can set limits (try sudo)");
    sys_procinfo_t t;
    if (sys_process_info(pid, &t) != 0 || t.state == PROC_TERMINATED) return cu_error("memlimit", "no such process");
    if (pid == 0 || pid == PID_SHELL) return cu_error("memlimit", "the idle task and the shell cannot be limited");
    if (!strcmp(argv[2], "off")) { limits[pid].set = false; screen_puts("limit removed\n"); return 0; }
    uint32_t v; if (!parse_bytes(argv[2], &v) || v == 0) return cu_error("memlimit", "the limit must be a number of bytes, or like 64K or 1M");
    limits[pid].set = true; limits[pid].start_tick = t.start_tick; limits[pid].limit = v;
    screen_puts("limit for process "); screen_put_dec((uint32_t)pid); screen_puts(" ("); screen_puts(t.name); screen_puts(") set to "); screen_put_dec(v);
    screen_puts(" bytes (accounting only)\n");
    if ((uint32_t)t.memory_used > v) screen_puts("it is already over the limit\n");
    return 0;
}

static const struct { const char *name; cmd_handler_t fn; } bindings[] = { { "bg", cmd_bg }, { "fg", cmd_fg }, { "memlimit", cmd_memlimit } };

int jobs_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
