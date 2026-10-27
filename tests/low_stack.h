#ifndef LOW_STACK_H
#define LOW_STACK_H
/* The system-call layer takes 32-bit pointers. On a 64-bit host the normal stack lives above 4 GiB, so
 * tests of commands (which pass local buffers to system calls) run on a static stack instead: a non-PIE
 * static array is always below 4 GiB, and every pointer then passes the real validators unchanged. */
#include <ucontext.h>
static char lc_stack[4u << 20] __attribute__((aligned(16)));
static ucontext_t lc_main_ctx, lc_low_ctx;
static int (*lc_fn)(void);
static int lc_rc;
static void lc_trampoline(void) { lc_rc = lc_fn(); }
static int run_on_low_stack(int (*fn)(void)) {
    lc_fn = fn;
    getcontext(&lc_low_ctx);
    lc_low_ctx.uc_stack.ss_sp = lc_stack; lc_low_ctx.uc_stack.ss_size = sizeof lc_stack;
    lc_low_ctx.uc_link = &lc_main_ctx;
    makecontext(&lc_low_ctx, lc_trampoline, 0);
    swapcontext(&lc_main_ctx, &lc_low_ctx);
    return lc_rc;
}
#endif
