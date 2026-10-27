/* Integration test: the REAL process commands (commands/process_commands.c) through the real
 * parser and registry, on the real process manager, scheduler and heap. Only the screen,
 * keyboard/timer and the final register swap are stubbed. Run with: make test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "include/shell.h"
#include "include/command.h"
#include "include/builtins.h"
#include "include/memory.h"
#include "include/filesystem.h"
#include "include/heap.h"
#include "include/process.h"
#include "include/scheduler.h"
#include "include/screen.h"
#include "include/timer.h"
#include "include/keyboard.h"
#include "include/workloads.h"


/* ---- Phase 9: the commands now go through system calls, which need these ---- */
#include "include/interrupts.h"
#include "include/syscall.h"
#include "include/process.h"
#include "tests/low_stack.h"
void interrupt_register(uint32_t v, interrupt_handler_t h) { (void)v; (void)h; }
static char out[32768]; static size_t out_len;
static void out_reset(void) { out_len = 0; out[0] = 0; }
void screen_init(void) {} void screen_clear(void) {} void screen_set_color(uint8_t f, uint8_t b) { (void)f; (void)b; }
void screen_backspace(void) {}
void screen_putc(char c) { if (out_len + 1 < sizeof out) { out[out_len++] = c; out[out_len] = 0; } }
void screen_puts(const char *s) { while (*s) screen_putc(*s++); }
void screen_put_dec(uint32_t v) { char b[16]; snprintf(b, sizeof b, "%u", v); screen_puts(b); }
void screen_put_hex(uint32_t v) { char b[16]; snprintf(b, sizeof b, "0x%08X", v); screen_puts(b); }
uint32_t timer_ticks(void) { return sched_now(); }
void timer_sleep_ms(uint32_t ms) { (void)ms; }
int keyboard_has_key(void) { return 1; }
int keyboard_getkey(void) { return 'q'; }
const char *exception_name(uint32_t v) { (void)v; return "exception"; }
int command_stub(const command_t *c, int argc, const char **argv) { (void)c; (void)argc; (void)argv; return 2; }

static int run, failed;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; printf("  FAIL line %d: %s\n    output was: [%s]\n", __LINE__, #cond, out); } } while (0)

static int sh(const char *line) {
    static char buf[SHELL_LINE_MAX * 2]; command_line_t cl;
    out_reset();
    if (parse_command_line(line, buf, sizeof buf, &cl) != PARSE_OK) return -1;
    const command_t *c = command_find(cl.argv[0]);
    if (!c || !c->handler) return -2;
    return c->handler(cl.argc, cl.argv);
}
static bool has(const char *s) { return strstr(out, s) != NULL; }
static int first_pid_after(const char *marker) { const char *p = strstr(out, marker); return p ? atoi(p + strlen(marker)) : -1; }

static multiboot_mmap_entry_t map_buf[1]; static multiboot_info_t mbi;

static void fresh(void) {
    map_buf[0] = (multiboot_mmap_entry_t){ 20, 0x100000, 0x4000000, 1 };
    memset(&mbi, 0, sizeof mbi);
    mbi.flags = MULTIBOOT_FLAG_MMAP; mbi.mmap_addr = (uint32_t)(uintptr_t)map_buf; mbi.mmap_length = sizeof map_buf;
    memory_init(&mbi, 0x100000, 0x110000);
    fs_set_clock(timer_ticks); fs_init();
    process_init();
    syscall_init();
}

static int real_main(void) {
    fresh();
    CHECK(process_commands_register() == 0);
    CHECK(command_find("ps") && command_find("ps")->handler && command_find("wait")->handler);

    puts("process commands (integration)");
    /* ps / pid / ppid */
    CHECK(sh("ps") == 0 && has("PID   NAME") && has("shell") && has("RUNNING") && !has("idle"));
    CHECK(sh("ps -a") == 0 && has("idle") && has("READY"));
    CHECK(sh("ps -x") == 1 && has("usage:"));
    CHECK(sh("pid") == 0 && strcmp(out, "1\n") == 0);
    CHECK(sh("ppid") == 0 && strcmp(out, "0\n") == 0);

    /* spawn: listing, creating, invalid input */
    CHECK(sh("spawn") == 0 && has("demo") && has("spin") && has("sleeper") && has("ticker") && has("crash"));
    CHECK(sh("spawn demo 4") == 0 && has("spawned process 2 'demo' (priority 10)"));
    CHECK(sh("spawn nothing") == 1 && has("unknown program 'nothing'"));
    CHECK(sh("spawn demo abc") == 1 && has("1 to 600"));
    CHECK(sh("spawn demo 0") == 1 && has("1 to 600"));
    CHECK(sh("spawn demo 601") == 1 && has("1 to 600"));
    CHECK(sh("spawn spin 5") == 1 && has("takes no argument"));
    CHECK(sh("spawn demo 1 2") == 1 && has("usage:"));
    CHECK(sh("spawn spin") == 0 && has("spawned process 3 'spin'"));
    CHECK(sh("nice 15 sleeper 2") == 0 && has("process 4") && has("priority 15"));
    CHECK(sh("nice 0 demo") == 1 && has("1-20"));
    CHECK(sh("nice 21 demo") == 1 && has("1-20"));
    CHECK(sh("nice 5") == 1 && has("usage:"));
    CHECK(sh("ps") == 0 && has("demo") && has("spin") && has("sleeper") && has("READY"));
    CHECK(sh("jobs") == 0 && has("[2]") && has("[3]") && has("[4]"));
    CHECK(sh("runqueue") == 0 && has("ready queue") && has("1. pid 2"));
    CHECK(sh("proccount") == 0 && has("READY") && has("RUNNING") && has("total"));

    /* kill: the exact messages the project spec asks for */
    CHECK(sh("kill 999") == 1 && strstr(out, "Error: process 999 does not exist.") != NULL);
    CHECK(sh("kill") == 1 && has("usage:"));
    CHECK(sh("kill abc") == 1 && has("not a valid process id"));
    CHECK(sh("kill -5") == 1 && has("not a valid process id"));
    CHECK(sh("kill 99999999999") == 1 && has("not a valid process id"));
    CHECK(sh("kill 1") == 1 && has("protected") && has("shell"));
    CHECK(sh("kill 0") == 1 && has("protected"));
    CHECK(sh("kill 3") == 0 && has("killed process 3 (spin)"));
    CHECK(sh("kill 3") == 1 && has("already terminated"));
    CHECK(sh("ps") == 0 && has("TERMINATED") && has("137"));

    /* wait: errors and reaping (a live child would block, so we only reap finished ones) */
    CHECK(sh("wait 999") == 1 && has("does not exist"));
    CHECK(sh("wait 1") == 1 && has("cannot wait for itself"));
    CHECK(sh("wait x") == 1 && has("not a valid process id"));
    CHECK(sh("wait 3") == 0 && has("process 3 finished with exit code 137 (killed)"));
    CHECK(sh("wait 3") == 1 && has("does not exist"));          /* already reaped */
    CHECK(sh("kill 2") == 0);
    CHECK(sh("kill 4") == 0);
    CHECK(sh("wait") == 0 && has("finished with exit code 137"));   /* any child */
    CHECK(sh("wait") == 0);
    CHECK(sh("wait") == 1 && has("no child processes"));

    /* pkill / killall / pgrep */
    CHECK(sh("spawn demo") == 0 && sh("spawn demo") == 0 && sh("spawn spin") == 0);
    CHECK(sh("pgrep demo") == 0 && strchr(out, '\n') != strrchr(out, '\n'));   /* two lines */
    CHECK(sh("pgrep nothing") == 1);
    CHECK(sh("pkill demo") == 0 && has("killed process") && sh("pgrep demo") == 0);   /* one demo left */
    CHECK(sh("killall demo") == 0 && has("killed process"));
    CHECK(sh("killall demo") == 1 && has("no running process named 'demo'"));
    CHECK(sh("pkill shell") == 1 && has("no running process"));    /* the shell is never a target */
    CHECK(sh("pkill") == 1 && has("usage:"));

    /* renice / priority */
    fresh();
    CHECK(sh("spawn demo") == 0);
    int pid = 2;
    CHECK(sh("priority 2") == 0 && has("priority 10") && has("recorded only"));
    CHECK(sh("renice 2 3") == 0 && has("now 3") && process_get(pid)->priority == 3);
    CHECK(sh("renice 2 0") == 1 && has("1-20") && process_get(pid)->priority == 3);
    CHECK(sh("renice 999 5") == 1 && has("does not exist"));
    CHECK(sh("renice 0 5") == 1 && has("protected"));
    CHECK(sh("renice 2") == 1 && has("usage:"));
    CHECK(sh("priority") == 0 && has("shell"));
    CHECK(sh("priority 999") == 1 && has("does not exist"));

    /* suspend / resume */
    CHECK(sh("suspend 2") == 0 && has("suspended") && process_get(2)->state == PROC_WAITING);
    CHECK(sh("ps") == 0 && has("(suspended)"));
    CHECK(sh("suspend 2") == 1 && has("not in a state"));
    CHECK(sh("runqueue") == 0 && has("(empty)"));
    CHECK(sh("resume 2") == 0 && has("resumed") && process_get(2)->state == PROC_READY);
    CHECK(sh("resume 2") == 1 && has("not suspended"));
    CHECK(sh("suspend 1") == 1 && has("protected"));
    CHECK(sh("suspend 999") == 1 && has("does not exist"));
    CHECK(sh("resume") == 1 && has("usage:"));

    /* scheduler views, quantum */
    CHECK(sh("sched") == 0 && has("round robin") && has("Time slice   : 5 ticks = 50 ms"));
    CHECK(sh("quantum") == 0 && has("5 ticks (50 ms)"));
    CHECK(sh("setquantum 8") == 0 && sched_timeslice() == 8 && sh("quantum") == 0 && has("8 ticks (80 ms)"));
    CHECK(sh("setquantum 0") == 1 && has("1-100") && sched_timeslice() == 8);
    CHECK(sh("setquantum 101") == 1 && sched_timeslice() == 8);
    CHECK(sh("setquantum abc") == 1 && sh("setquantum") == 1);
    sh("setquantum 5");
    CHECK(sh("yield") == 0 && process_current_pid() == 2);       /* the stub switches immediately; pid 2 now owns the CPU */
    schedule();                                                  /* hand the CPU back to the shell */
    CHECK(process_current_pid() == PID_SHELL);
    CHECK(sh("ctxstat") == 0 && has("switches total") && has("voluntary"));

    /* pinfo / pstree / top */
    CHECK(sh("pinfo 2") == 0 && has("Process 2 'demo'") && has("kernel stack") && has("CPU ticks"));
    CHECK(sh("pinfo 999") == 1 && has("does not exist"));
    CHECK(sh("pinfo") == 0 && has("'shell'"));
    CHECK(sh("pstree") == 0 && has("idle(0)") && has("shell(1)") && has("demo(2)"));
    CHECK(sh("top 1") == 0 && has("MiniOS top") && has("shell"));
    CHECK(sh("top 0") == 1 && has("1-3600"));
    CHECK(sh("top") == 0);                                       /* stub keyboard has a key ready -> quits at once */

    /* procdemo and table exhaustion */
    fresh();
    CHECK(sh("procdemo") == 0 && has("started 3 CPU-bound") && process_count_state(PROC_READY) == 4);   /* 3 demos + the idle task */
    CHECK(sh("ps") == 0 && has("demo-a") && has("demo-b") && has("demo-c"));
    int n = 0; while (sh("spawn spin") == 0) n++;
    CHECK(n == PROC_MAX - 2 - 3 && has("process table is full"));
    CHECK(sh("procdemo") == 1 && has("process table is full"));

    /* a process that crashes: only it dies (fault hook used by the kernel) */
    fresh();
    CHECK(sh("spawn crash") == 0);
    int cp = first_pid_after("spawned process ");
    for (int i = 0; i < 4 && process_current_pid() != cp; i++) schedule();
    CHECK(process_current_pid() == cp);
    registers_t regs; memset(&regs, 0, sizeof regs); regs.int_no = 0; regs.eip = 0x123456;
    extern int stub_switches; int sw = stub_switches; (void)sw;
    {
        extern jmp_buf *stub_jump; jmp_buf jb; stub_jump = &jb;
        out_reset();
        if (setjmp(jb) == 0) process_fault_hook(&regs);
        stub_jump = NULL;
    }
    CHECK(has("terminated: exception") && has("EIP=0x00123456"));
    CHECK(process_get(cp)->state == PROC_TERMINATED && process_get(cp)->exit_code == PROC_EXIT_FAULT_BASE + 0 && process_current_pid() == PID_SHELL);
    sched_finish_switch();
    CHECK(sh("wait") == 0 && has("exit code 128 (CPU exception)"));    /* 128 + vector 0 = divide error */
    /* the shell itself faulting must NOT be swallowed */
    CHECK(process_fault_hook(&regs) == false);

    CHECK(heap_check(NULL) == HEAP_OK);
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}

int main(void) { return run_on_low_stack(real_main); }
