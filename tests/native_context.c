/* Native test of the REAL context-switch assembly (boot/context.S) and stack frame builder
 * (kernel/cpu/context.c). Runs as a freestanding 32-bit Linux program: no libc, raw syscalls.
 * Checks, over 200000 round trips between two tasks and the main stack:
 *   - a brand-new task really starts in its entry function on its own stack
 *   - the entry function sees an ABI-aligned stack
 *   - ebx, esi, edi, ebp survive every switch (callee-saved contract)
 *   - control alternates strictly main -> A -> main -> B -> main ...
 * Build/run: make test-context  (needs a 32-bit capable gcc and kernel; skipped otherwise) */
#include "include/types.h"
#include "include/arch_context.h"

#define ROUNDS 100000

static long sys3(long n, long a, long b, long c) {
    long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory"); return r;
}
static void say(const char *s) { long n = 0; while (s[n]) n++; sys3(4, 1, (long)s, n); }
static void quit(int code) { sys3(1, code, 0, 0); for (;;) {} }

static uint8_t stack_a[8192] __attribute__((aligned(16)));
static uint8_t stack_b[8192] __attribute__((aligned(16)));
static uint32_t main_esp, a_esp, b_esp;
static volatile int turn_log[8], log_n, misaligned, reg_errors, a_rounds, b_rounds, a_started, b_started;

/* int yield_checked(uint32_t *save, uint32_t target, uint32_t magic)
 * Loads magic into ebx/esi/edi/ebp, switches away, and on return returns 1 if any of them changed. */
__asm__(
    ".text\n.global yield_checked\nyield_checked:\n"
    "  push %ebp\n  push %ebx\n  push %esi\n  push %edi\n"
    "  mov 20(%esp), %eax\n  mov 24(%esp), %edx\n  mov 28(%esp), %ecx\n"
    "  mov %ecx, %ebx\n  mov %ecx, %esi\n  mov %ecx, %edi\n  mov %ecx, %ebp\n"
    "  push %edx\n  push %eax\n  call arch_context_switch\n  add $8, %esp\n"
    "  mov 28(%esp), %ecx\n  xor %eax, %eax\n"
    "  cmp %ecx, %ebx\n  jne 1f\n  cmp %ecx, %esi\n  jne 1f\n  cmp %ecx, %edi\n  jne 1f\n  cmp %ecx, %ebp\n  jne 1f\n"
    "  jmp 2f\n1: mov $1, %eax\n2:\n"
    "  pop %edi\n  pop %esi\n  pop %ebx\n  pop %ebp\n  ret\n");
int yield_checked(uint32_t *save, uint32_t target, uint32_t magic);

static void note(int who) { if (log_n < 8) turn_log[log_n++] = who; }
static void check_align(void) { if (((uintptr_t)__builtin_frame_address(0) & 15) != 8) misaligned++; }

static void task_a(void) {
    check_align(); a_started = 1;
    for (;;) { note(1); a_rounds++; reg_errors += yield_checked(&a_esp, main_esp, 0xA1A1A1A1u); }
}
static void task_b(void) {
    check_align(); b_started = 1;
    for (;;) { note(2); b_rounds++; reg_errors += yield_checked(&b_esp, main_esp, 0xB2B2B2B2u); }
}

void _start(void) {
    a_esp = arch_stack_init(stack_a + sizeof stack_a, task_a);
    b_esp = arch_stack_init(stack_b + sizeof stack_b, task_b);
    if (a_esp < (uint32_t)(uintptr_t)stack_a || a_esp >= (uint32_t)(uintptr_t)(stack_a + sizeof stack_a)) { say("FAIL: initial esp outside stack\n"); quit(1); }

    for (int i = 0; i < ROUNDS; i++) {
        note(0); reg_errors += yield_checked(&main_esp, a_esp, 0x0F0F0F0Fu);
        note(0); reg_errors += yield_checked(&main_esp, b_esp, 0x3C3C3C3Cu);
    }
    int bad = 0;
    if (!a_started || !b_started)        { say("FAIL: a task never started\n"); bad = 1; }
    if (a_rounds != ROUNDS || b_rounds != ROUNDS) { say("FAIL: wrong number of rounds\n"); bad = 1; }
    if (misaligned)                      { say("FAIL: entry stack not 16-byte aligned\n"); bad = 1; }
    if (reg_errors)                      { say("FAIL: callee-saved registers were clobbered\n"); bad = 1; }
    const int expect[8] = { 0, 1, 0, 2, 0, 1, 0, 2 };
    for (int i = 0; i < 8; i++) if (turn_log[i] != expect[i]) { say("FAIL: wrong task order\n"); bad = 1; break; }
    if (bad) quit(1);
    say("context switch OK: 200000 round trips, registers preserved, stacks aligned\n");
    quit(0);
}
