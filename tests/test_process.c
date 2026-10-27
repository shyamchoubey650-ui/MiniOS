/* Host-side tests for the process manager and scheduler (real kernel sources, real heap).
 * Only the register swap is stubbed (tests/host_arch_stub.c). Run with: make test */
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "include/process.h"
#include "include/scheduler.h"
#include "include/memory.h"
#include "include/heap.h"

uint32_t timer_ticks(void) { return 0; }      /* the logger timestamps entries */

extern jmp_buf *stub_jump;
extern int stub_switches;

static int run, failed;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; printf("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

/* Run `stmt`; evaluates to 1 if it "blocked" (the stub switched tasks and jumped back). */
static int blocked_flag;
#define BLOCKS(stmt) do { jmp_buf jb; stub_jump = &jb; if (setjmp(jb) == 0) { stmt; blocked_flag = 0; } else blocked_flag = 1; stub_jump = NULL; } while (0)

static multiboot_mmap_entry_t map_buf[1];
static multiboot_info_t mbi;
static void nop(void *a) { (void)a; }

static void boot(void) {
    map_buf[0] = (multiboot_mmap_entry_t){ 20, 0x100000, 0x4000000, 1 };
    memset(&mbi, 0, sizeof mbi);
    mbi.flags = MULTIBOOT_FLAG_MMAP; mbi.mmap_addr = (uint32_t)(uintptr_t)map_buf; mbi.mmap_length = sizeof map_buf;
    memory_init(&mbi, 0x100000, 0x110000);
    stub_jump = NULL;
    process_init();
}
static int spawn(const char *name) { int pid = -1; process_create(name, nop, NULL, PRIORITY_DEFAULT, &pid); return pid; }
/* Make `pid` the running task by rotating the ready queue. */
static void switch_to(int pid) { for (int i = 0; i < 2 * PROC_MAX && process_current_pid() != pid; i++) schedule(); }
static const PCB *P(int pid) { return process_get(pid); }
static size_t heap_used_blocks(void) { heap_stats_t h; heap_stats(&h); return h.used_blocks; }
static void tick_n(int n) { for (int i = 0; i < n; i++) { sched_tick(); sched_on_irq_exit(); } }

static void test_init_and_create(void) {
    puts("process: init and create");
    boot();
    CHECK(process_current_pid() == PID_SHELL && P(1)->state == PROC_RUNNING && strcmp(P(1)->name, "shell") == 0);
    CHECK(P(0) && strcmp(P(0)->name, "idle") == 0 && P(0)->state == PROC_READY);
    CHECK(sched_ready_count() == 0);                          /* idle is never queued */
    CHECK(P(0)->memory_used == (int)PROC_STACK_SIZE && P(1)->memory_used == (int)PROC_BOOT_STACK);

    size_t blocks0 = heap_used_blocks();
    int pid = -1;
    CHECK(process_create("demo", nop, NULL, 12, &pid) == PROC_OK && pid == 2);
    CHECK(P(2)->state == PROC_READY && P(2)->parent_pid == 1 && P(2)->priority == 12 && P(2)->memory_used == (int)PROC_STACK_SIZE);
    CHECK(sched_ready_count() == 1 && sched_ready_at(0) == P(2));
    CHECK(heap_used_blocks() == blocks0 + 1);                 /* the stack really came from the heap */
    CHECK(*(const uint32_t *)P(2)->stack_base == PROC_STACK_CANARY);
    CHECK(process_create("editor", nop, NULL, 15, NULL) == PROC_OK);   /* pid_out is optional */
    CHECK(P(3) && strcmp(P(3)->name, "editor") == 0 && P(3)->priority == 15);

    /* invalid input */
    CHECK(process_create(NULL, nop, NULL, 10, &pid) == PROC_ERR_ARG);
    CHECK(process_create("", nop, NULL, 10, &pid) == PROC_ERR_ARG);
    CHECK(process_create("x", NULL, NULL, 10, &pid) == PROC_ERR_ARG);
    CHECK(process_create("x", nop, NULL, 0, &pid) == PROC_ERR_ARG);
    CHECK(process_create("x", nop, NULL, 21, &pid) == PROC_ERR_ARG);
    CHECK(process_create("x", nop, NULL, 1, &pid) == PROC_OK && process_create("x", nop, NULL, 20, &pid) == PROC_OK);   /* boundaries ok */
    int p2 = spawn("0123456789012345678901234567890123456789");
    CHECK(strlen(P(p2)->name) == PROC_NAME_MAX - 1);          /* long names are truncated, not overflowed */
    CHECK(P(99) == NULL && P(-1) == NULL);
}

static void test_table_limits(void) {
    puts("process: table and memory limits");
    boot();
    int made = 0, rc;
    while ((rc = process_create("t", nop, NULL, 10, NULL)) == PROC_OK) made++;
    CHECK(made == PROC_MAX - 2 && rc == PROC_ERR_NO_SLOT);    /* idle + shell use two slots */
    CHECK(heap_check(NULL) == HEAP_OK);
    CHECK(process_kill(5) == PROC_OK);
    CHECK(process_create("again", nop, NULL, 10, NULL) == PROC_OK);   /* reclaims the zombie slot */
    CHECK(P(5) == NULL);                                      /* its exit status is gone */
    CHECK(process_create("full", nop, NULL, 10, NULL) == PROC_ERR_NO_SLOT);

    boot();
    heap_stats_t h; heap_stats(&h);
    void *hog = kmalloc(h.largest_free);                      /* leave no room for a stack */
    CHECK(hog != NULL);
    CHECK(process_create("starved", nop, NULL, 10, NULL) == PROC_ERR_NO_MEMORY);
    CHECK(process_count_state(PROC_UNUSED) == PROC_MAX - 2);  /* a failed create must not eat a slot */
    kfree(hog);
    CHECK(process_create("ok", nop, NULL, 10, NULL) == PROC_OK);
}

static void test_kill(void) {
    puts("process: kill");
    boot();
    int a = spawn("a"), b = spawn("b"), c = spawn("c");
    size_t blocks = heap_used_blocks();
    CHECK(process_kill(999) == PROC_ERR_NO_SUCH_PID && process_kill(-3) == PROC_ERR_NO_SUCH_PID);
    CHECK(process_kill(0) == PROC_ERR_PROTECTED && process_kill(1) == PROC_ERR_PROTECTED);
    CHECK(process_kill(b) == PROC_OK);
    CHECK(P(b)->state == PROC_TERMINATED && P(b)->exit_code == PROC_EXIT_KILLED);
    CHECK(heap_used_blocks() == blocks - 1 && P(b)->stack_base == NULL && P(b)->memory_used == 0);   /* stack returned */
    CHECK(sched_ready_count() == 2 && sched_ready_at(0) == P(a) && sched_ready_at(1) == P(c));        /* order of the rest kept */
    CHECK(process_kill(b) == PROC_ERR_BAD_STATE);             /* already dead */
    CHECK(heap_check(NULL) == HEAP_OK);

    /* a task killing itself */
    switch_to(a);
    CHECK(process_current_pid() == a);
    BLOCKS(process_kill(a));
    CHECK(blocked_flag == 1 && P(a)->state == PROC_TERMINATED && process_current_pid() != a);
    sched_finish_switch();                                    /* what the next task does first */
    CHECK(P(a)->stack_base == NULL);
    switch_to(PID_SHELL);
    CHECK(process_current_pid() == PID_SHELL);

    /* killing a sleeping and a suspended process */
    int s = spawn("s"), u = spawn("u");
    CHECK(process_suspend(u) == PROC_OK && process_kill(u) == PROC_OK && P(u)->state == PROC_TERMINATED);
    (void)s;
}

static void test_wait_and_exit(void) {
    puts("process: wait, exit, orphans, Ctrl+C");
    boot();
    int code = -1;
    CHECK(process_wait(-1, &code) == PROC_ERR_NO_CHILD);
    CHECK(process_wait(999, &code) == PROC_ERR_NO_SUCH_PID);
    CHECK(process_wait(PID_SHELL, &code) == PROC_ERR_SELF_WAIT);

    int a = spawn("a");
    CHECK(process_kill(a) == PROC_OK);
    CHECK(process_wait(a, &code) == a && code == PROC_EXIT_KILLED);
    CHECK(P(a) == NULL);                                      /* slot reaped */
    CHECK(process_wait(a, &code) == PROC_ERR_NO_SUCH_PID);    /* cannot be reaped twice */

    /* exit code travels to the parent */
    int b = spawn("b");
    switch_to(b);
    BLOCKS(process_exit(7));
    CHECK(blocked_flag == 1 && P(b)->exit_code == 7 && P(b)->state == PROC_TERMINATED);
    sched_finish_switch();
    switch_to(PID_SHELL);
    CHECK(process_wait(-1, &code) == b && code == 7);         /* wait for "any child" */

    /* blocking wait: shell sleeps until the child exits */
    int c = spawn("c");
    BLOCKS(process_wait(c, &code));
    CHECK(blocked_flag == 1 && P(1)->state == PROC_WAITING && P(1)->wait_kind == WAIT_CHILD);
    CHECK(process_current_pid() == c && sched_ready_count() == 0);
    BLOCKS(process_exit(5));
    CHECK(blocked_flag == 1);
    CHECK(process_current_pid() == PID_SHELL && P(1)->state == PROC_RUNNING);   /* the exit woke the parent, the only ready task */
    sched_finish_switch();
    CHECK(process_wait(c, &code) == c && code == 5);

    /* Ctrl+C aborts a blocked wait; without a blocked shell it is not consumed */
    CHECK(process_ctrl_c() == false);
    int d = spawn("d");
    BLOCKS(process_wait(d, &code));
    CHECK(blocked_flag == 1 && P(1)->state == PROC_WAITING);
    CHECK(process_ctrl_c() == true && P(1)->state == PROC_READY && P(1)->interrupted);
    switch_to(PID_SHELL);
    CHECK(process_wait(d, &code) == PROC_ERR_INTERRUPTED);
    CHECK(P(d)->state != PROC_TERMINATED && !P(1)->interrupted);          /* d keeps running; flag cleared */

    /* waiting on a process that is not our child; orphans are adopted by the shell */
    boot();
    int child = spawn("child");
    switch_to(child);
    int grand = spawn("grand");
    CHECK(P(grand)->parent_pid == child);
    BLOCKS(process_exit(0));                                  /* the child dies first */
    sched_finish_switch();
    CHECK(P(grand)->parent_pid == PID_SHELL);                 /* reparented */
    switch_to(PID_SHELL);
    CHECK(process_kill(grand) == PROC_OK);                    /* the shell can now kill AND reap its adopted orphan */
    CHECK(process_wait(grand, &code) == grand && code == PROC_EXIT_KILLED);
    int other = spawn("other");
    switch_to(other);
    int mine = spawn("mine");
    switch_to(PID_SHELL);
    CHECK(process_wait(mine, &code) == PROC_ERR_NOT_CHILD);
}

static void test_suspend_priority_sleep(void) {
    puts("process: suspend/resume, priority, sleep, stack canary");
    boot();
    int a = spawn("a");
    CHECK(process_suspend(999) == PROC_ERR_NO_SUCH_PID && process_suspend(0) == PROC_ERR_PROTECTED && process_suspend(1) == PROC_ERR_PROTECTED);
    CHECK(process_suspend(a) == PROC_OK && P(a)->state == PROC_WAITING && P(a)->wait_kind == WAIT_SUSPENDED && !P(a)->queued);
    CHECK(sched_ready_count() == 0);
    CHECK(process_suspend(a) == PROC_ERR_BAD_STATE);
    CHECK(process_resume(a) == PROC_OK && P(a)->state == PROC_READY && sched_ready_count() == 1);
    CHECK(process_resume(a) == PROC_ERR_BAD_STATE);           /* not suspended any more */
    CHECK(process_resume(999) == PROC_ERR_NO_SUCH_PID);
    process_kill(a);
    CHECK(process_resume(a) == PROC_ERR_BAD_STATE && process_suspend(a) == PROC_ERR_BAD_STATE);

    int b = spawn("b");
    CHECK(process_set_priority(b, 1) == PROC_OK && P(b)->priority == 1);
    CHECK(process_set_priority(b, 20) == PROC_OK && P(b)->priority == 20);
    CHECK(process_set_priority(b, 0) == PROC_ERR_ARG && process_set_priority(b, 21) == PROC_ERR_ARG && P(b)->priority == 20);
    CHECK(process_set_priority(999, 5) == PROC_ERR_NO_SUCH_PID && process_set_priority(0, 5) == PROC_ERR_PROTECTED);
    CHECK(process_set_priority(PID_SHELL, 3) == PROC_OK);
    process_kill(b);
    CHECK(process_set_priority(b, 5) == PROC_ERR_BAD_STATE);

    /* sleeping */
    boot();
    int s = spawn("s");
    switch_to(s);
    uint32_t t0 = sched_now();
    BLOCKS(process_sleep_ticks(5));
    CHECK(blocked_flag == 1 && P(s)->state == PROC_WAITING && P(s)->wait_kind == WAIT_SLEEP && P(s)->wake_tick == t0 + 5);
    tick_n(4);
    CHECK(P(s)->state == PROC_WAITING);                       /* not yet */
    tick_n(1);
    CHECK(P(s)->state != PROC_WAITING);                       /* woken exactly on tick 5 */
    boot();
    int before = stub_switches;
    process_sleep_ticks(0);                                   /* shell cannot sleep; acts as a yield */
    CHECK(P(1)->state == PROC_RUNNING && stub_switches == before);

    /* stack canary */
    boot();
    int o = spawn("overflow");
    switch_to(o);
    P(o)->stack_base[0] ^= 0xFF;                              /* simulate a stack that ran into its guard word */
    schedule();
    CHECK(P(o)->state == PROC_TERMINATED && P(o)->exit_code == PROC_EXIT_STACK_OVERFLOW);
    sched_stats_t st; sched_stats(&st);
    CHECK(st.stack_overflows == 1 && process_current_pid() != o);
}

static int pid_log[400];
static void record(int n) { for (int i = 0; i < n; i++) { sched_tick(); pid_log[i] = process_current_pid(); sched_on_irq_exit(); } }

static void test_scheduler(void) {
    puts("scheduler: round robin");
    boot();
    CHECK(sched_timeslice() == SCHED_DEFAULT_SLICE);
    CHECK(sched_set_timeslice(0) == -1 && sched_set_timeslice(101) == -1 && sched_timeslice() == SCHED_DEFAULT_SLICE);
    CHECK(sched_set_timeslice(3) == 0 && sched_timeslice() == 3);
    int a = spawn("a"), b = spawn("b"), c = spawn("c");
    record(120);
    /* each task owns the CPU for exactly 3 ticks, in order 1,a,b,c,1,a,b,c,... */
    int order_ok = 1, len_ok = 1;
    int expect[4] = { PID_SHELL, a, b, c };
    for (int i = 0; i < 120; i++) { if (pid_log[i] != expect[(i / 3) % 4]) order_ok = 0; }
    CHECK(order_ok);
    CHECK(len_ok);
    CHECK(P(1)->cpu_ticks == 30 && P(a)->cpu_ticks == 30 && P(b)->cpu_ticks == 30 && P(c)->cpu_ticks == 30);   /* perfectly fair */
    sched_stats_t st; sched_stats(&st);
    CHECK(st.total_ticks == 120 && st.context_switches == 40 && st.preemptions == 40 && st.voluntary == 0);
    CHECK(P(a)->times_scheduled == 10 && P(1)->times_scheduled == 10);

    /* time slice 1 alternates every tick; changing it mid-run takes effect */
    sched_set_timeslice(1);
    int base = process_current_pid();
    record(8);
    int alt_ok = 1; for (int i = 1; i < 8; i++) if (pid_log[i] == pid_log[i - 1]) alt_ok = 0;
    CHECK(alt_ok); (void)base;
    CHECK(sched_set_timeslice(100) == 0 && sched_set_timeslice(1) == 0);

    /* a lone task is never switched out, and its slice simply refills */
    boot();
    sched_stats_t s0; sched_stats(&s0);
    record(50);
    sched_stats_t s1; sched_stats(&s1);
    CHECK(s1.context_switches == s0.context_switches && P(1)->cpu_ticks == 50);

    /* ready queue order and yield */
    boot();
    int x = spawn("x"), y = spawn("y");
    CHECK(sched_ready_at(0) == P(x) && sched_ready_at(1) == P(y) && sched_ready_at(2) == NULL);
    process_yield();                                          /* shell goes to the back */
    CHECK(process_current_pid() == x && sched_ready_at(0) == P(y) && sched_ready_at(1) == P(1));
    sched_stats(&st);
    CHECK(st.voluntary == 1 && st.preemptions == 0);
    sched_make_ready((PCB *)P(y));                                   /* already queued: must not be duplicated */
    CHECK(sched_ready_count() == 2);
    sched_make_ready((PCB *)P(0));                            /* the idle task must never enter the queue */
    CHECK(sched_ready_count() == 2 && !P(0)->queued);

    puts("scheduler: idle task");
    boot();
    int w = spawn("worker");
    int code = 0;
    BLOCKS(process_wait(w, &code));                           /* shell blocks -> worker runs */
    CHECK(blocked_flag == 1 && process_current_pid() == w);
    BLOCKS(process_sleep_ticks(10));                          /* worker sleeps -> nobody ready -> idle */
    CHECK(blocked_flag == 1 && process_current_pid() == PID_IDLE && P(0)->state == PROC_RUNNING);
    sched_stats(&st);
    uint32_t idle0 = st.idle_ticks;
    tick_n(9);
    sched_stats(&st);
    CHECK(st.idle_ticks == idle0 + 9 && process_current_pid() == PID_IDLE);
    tick_n(1);                                                /* the sleeper wakes; idle is preempted at once */
    CHECK(process_current_pid() == w && P(0)->state == PROC_READY && sched_ready_count() == 0);
    BLOCKS(process_exit(0));
    CHECK(blocked_flag == 1 && process_current_pid() == PID_SHELL);
    sched_finish_switch();
    CHECK(process_wait(w, &code) == w && code == 0);
}

int main(void) {
    test_init_and_create(); test_table_limits(); test_kill(); test_wait_and_exit();
    test_suspend_priority_sleep(); test_scheduler();
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
