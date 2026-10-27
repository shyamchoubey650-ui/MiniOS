/* MiniOS Phase 16 - kernel entry.
 * Boot sequence: multiboot check, GDT/IDT/PIC, memory manager (frames + heap), timer,
 * keyboard, process manager + scheduler, MiniFS, system calls, user manager, command registry, then the shell (PID 1).
 * The kernel log (include/logger.h) starts first of all, so every boot message below is also stored in it (see `dmesg`). */
#include "include/types.h"
#include "include/string.h"
#include "include/multiboot.h"
#include "include/screen.h"
#include "include/keyboard.h"
#include "include/timer.h"
#include "include/interrupts.h"
#include "include/shell.h"
#include "include/command.h"
#include "include/builtins.h"
#include "include/memory.h"
#include "include/heap.h"
#include "include/pmm.h"
#include "include/process.h"
#include "include/scheduler.h"
#include "include/workloads.h"
#include "include/filesystem.h"
#include "include/syscall.h"
#include "include/users.h"
#include "include/kinfo.h"
#include "include/ramdisk.h"
#include "include/logger.h"
#include "include/mouse.h"
#include "include/version.h"

extern uint8_t kernel_start[], kernel_end[], kernel_data_start[];


static void ok(const char *msg) {
    screen_puts("[ ");
    screen_set_color(VGA_LIGHT_GREEN, VGA_BLACK); screen_puts("OK");
    screen_set_color(VGA_LIGHT_GREY, VGA_BLACK);  screen_puts(" ] ");
    screen_puts(msg); screen_putc('\n');
    klog(LOG_INFO, "boot", msg);
}

static void ok_begin(void) {
    screen_puts("[ ");
    screen_set_color(VGA_LIGHT_GREEN, VGA_BLACK); screen_puts("OK");
    screen_set_color(VGA_LIGHT_GREY, VGA_BLACK);  screen_puts(" ] ");
}

static void fail(const char *msg) {
    screen_puts("[");
    screen_set_color(VGA_LIGHT_RED, VGA_BLACK); screen_puts("FAIL");
    screen_set_color(VGA_LIGHT_GREY, VGA_BLACK); screen_puts("] ");
    screen_puts(msg); screen_putc('\n');
    klog(LOG_ERROR, "boot", msg);
}

/* Breakpoint (int3) is recoverable: handler just reports and returns. */
static void breakpoint_handler(registers_t *r) {
    screen_puts("  [INT 3] breakpoint caught at EIP="); screen_put_hex(r->eip);
    screen_puts(" - execution resumes\n");
}

static volatile int selftest_flag;
static void selftest_task(void *arg) { selftest_flag = (int)(uintptr_t)arg; }

void kernel_main(uint32_t magic, multiboot_info_t *mbi) {
    klog_init();
    screen_init();
    screen_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    screen_puts("MiniOS v" MINIOS_VERSION " - " MINIOS_PHASE "\n");
    screen_puts("--------------------------------\n");
    screen_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) { fail("Bad multiboot magic"); for (;;) __asm__("hlt"); }
    ok("Multiboot magic verified");

    if (mbi->flags & MULTIBOOT_FLAG_MEM) {
        screen_puts("[ "); screen_set_color(VGA_LIGHT_GREEN, VGA_BLACK); screen_puts("OK");
        screen_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        screen_puts(" ] Memory: "); screen_put_dec((mbi->mem_upper + 1024) / 1024);
        screen_puts(" MiB (approx)\n");
        klogf(LOG_INFO, "boot", "Memory: %u MiB (approx)", (uint32_t)((mbi->mem_upper + 1024) / 1024));
    }
    screen_puts("[ "); screen_set_color(VGA_LIGHT_GREEN, VGA_BLACK); screen_puts("OK");
    screen_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    screen_puts(" ] Kernel at "); screen_put_hex((uint32_t)kernel_start);
    screen_puts(" - "); screen_put_hex((uint32_t)kernel_end); screen_putc('\n');
    klogf(LOG_INFO, "boot", "Kernel at %x - %x", (uint32_t)kernel_start, (uint32_t)kernel_end);

    kinfo_set_boot(mbi);                       /* remembered for the 'bootinfo' command */
    kinfo_set_kernel((uintptr_t)kernel_start, (uintptr_t)kernel_data_start, (uintptr_t)kernel_end);
    interrupts_init();                         /* GDT + PIC remap + IDT */
    ok("GDT loaded (null, code, data)");
    ok("PIC remapped to vectors 32-47");
    ok("IDT loaded (32 exceptions + 16 IRQs)");

    interrupt_register(3, breakpoint_handler);

    /* Memory manager: physical frames from the Multiboot map, then the kernel heap. */
    int mr = memory_init(mbi, (uintptr_t)kernel_start, (uintptr_t)kernel_end);
    if (mr != MEM_OK) {
        fail("Memory manager failed:");
        screen_puts("       "); screen_puts(memory_strerror(mr)); screen_putc('\n');
        for (;;) __asm__ volatile ("cli; hlt");
    }
    memory_summary_t ms; memory_summary(&ms);
    pmm_stats_t ps = pmm_stats();
    ok_begin(); screen_puts("Physical memory: "); screen_put_dec(ms.ram_bytes >> 20); screen_puts(" MiB usable in ");
    screen_put_dec(ps.ram_frames); screen_puts(" frames (map: "); screen_puts(memory_map_source()); screen_puts(")\n");
    klogf(LOG_INFO, "boot", "Physical memory: %u MiB usable in %u frames", ms.ram_bytes >> 20, (uint32_t)ps.ram_frames);
    ok_begin(); screen_puts("Kernel heap: "); screen_put_dec(ms.heap_size >> 10); screen_puts(" KiB at ");
    screen_put_hex(ms.heap_base); screen_putc('\n');
    klogf(LOG_INFO, "boot", "Kernel heap: %u KiB at %x", (uint32_t)(ms.heap_size >> 10), ms.heap_base);

    /* Logger self-test: the boot messages written so far must be readable back, starting with entry number 1. */
    {
        log_entry_t first; int n = klog_get(0, true, NULL);
        if (n >= 5 && klog_get(0, true, &first) == 0 && first.seq == 1 && !strcmp(first.src, "boot")) ok("Kernel log: 256-entry ring buffer (read it with 'dmesg' and 'logs')");
        else fail("Kernel log: self-test FAILED");
    }

    /* Heap self-test: allocate, free, and make sure the heap looks the same as before. */
    {
        heap_stats_t h0, h1; size_t bad;
        heap_stats(&h0);
        void *a = kmalloc(100), *b = kmalloc(5000), *c = kmalloc(1);
        bool good = a && b && c && kfree(b) == HEAP_OK && kfree(a) == HEAP_OK && kfree(c) == HEAP_OK;
        heap_stats(&h1);
        good = good && heap_check(&bad) == HEAP_OK && h1.used_blocks == h0.used_blocks &&
               h1.payload_used == h0.payload_used && h1.block_count == h0.block_count;
        if (good) ok("Heap self-test (alloc, free, coalesce, integrity check)");
        else fail("Heap self-test");
    }

    timer_init(TIMER_HZ);
    keyboard_init();
    {   /* the mouse is optional: its wheel scrolls the console. A missing mouse is not an error. */
        mouse_info_t mi;
        if (mouse_init()) { mouse_info(&mi); ok(mi.wheel ? "PS/2 mouse found: the wheel scrolls the console (click the window to capture it)" : "PS/2 mouse found (no wheel: scroll with PageUp/PageDown)"); }
        else ok("No PS/2 mouse: scroll with PageUp/PageDown or Ctrl+Up/Down");
    }
    if (ramdisk_init() == 0) { ok_begin(); screen_put_dec(RAMDISK_SECTORS * RAMDISK_SECTOR / 1024); screen_puts(" KiB RAM-disk (block device, volatile)\n"); klogf(LOG_INFO, "boot", "RAM-disk: %u KiB block device (volatile)", (uint32_t)(RAMDISK_SECTORS * RAMDISK_SECTOR / 1024)); }
    else fail("RAM-disk allocation");

    /* Process manager: PID 0 = idle task, PID 1 = this code (the shell). Hooks must be in
     * place before interrupts are enabled, because the timer IRQ drives preemption. */
    process_init();
    klog_set_pid_source(process_current_pid);
    timer_set_tick_hook(sched_tick);
    interrupts_set_exit_hook(sched_on_irq_exit);
    interrupts_set_fault_hook(process_fault_hook);
    keyboard_set_ctrl_c_hook(process_ctrl_c);
    interrupts_enable();

    /* Self-test: prove IRQ0 is really firing by sleeping 100 ms. */
    uint32_t before = timer_ticks();
    timer_sleep_ms(100);
    uint32_t delta = timer_ticks() - before;
    if (delta >= 9 && delta <= 20) ok("Timer IRQ0 self-test (100 ms = ~10 ticks)");
    else fail("Timer IRQ0 self-test");
    ok("Keyboard IRQ1 ready (interrupt-driven)");

    /* Scheduler self-test: start a real task, let the timer preempt the shell into it, and
     * check that it ran on its own stack, exited, and that its stack was returned to the heap. */
    {
        heap_stats_t h0, h1; int pid = -1, code = -1;
        heap_stats(&h0);
        selftest_flag = 0;
        bool good = process_create("selftest", selftest_task, (void *)(uintptr_t)42, PRIORITY_DEFAULT, &pid) == PROC_OK;
        if (good) timer_sleep_ms(150);                         /* the shell halts; the timer switches to the task */
        good = good && selftest_flag == 42 && process_wait(pid, &code) == pid && code == 0;
        heap_stats(&h1);
        good = good && h1.used_blocks == h0.used_blocks && h1.payload_used == h0.payload_used;
        if (good) ok("Scheduler self-test (real context switch, stack freed)");
        else fail("Scheduler self-test");
    }
    {
        /* MiniFS: RAM-backed file system with the standard directory layout. */
        fs_set_clock(timer_ticks);
        int frc = fs_init();
        fs_check_report_t fr; fs_usage_t fu; fs_usage(&fu);
        if (frc == FS_OK && fs_check(&fr) == 0) {
            ok_begin(); screen_puts("File system: MiniFS (RAM-backed, volatile), "); screen_put_dec(fu.dirs); screen_puts(" directories, ");
            screen_put_dec(fu.files); screen_puts(" files, consistent\n");
            klogf(LOG_INFO, "boot", "File system: MiniFS, %u directories, %u files, consistent", (uint32_t)fu.dirs, (uint32_t)fu.files);
        } else fail("File system (MiniFS) failed its consistency check");
    }
    {
        /* System calls: int 0x80. Kernel code and read-only data are write-protected for call arguments. */
        memory_set_code_range((uintptr_t)kernel_start, (uintptr_t)kernel_data_start);
        syscall_init();
        /* Self-test through the REAL trap: getpid, then open/read/close of a file, then a bad pointer. */
        char host[16]; sys_stats_t before, after;
        sys_stats(&before);
        int pid = sys_getpid();
        int fd = sys_open("/etc/hostname", SYS_O_READ);
        int n = fd >= 0 ? sys_read(fd, host, 6) : -1;
        int cl = fd >= 0 ? sys_close(fd) : -1;
        int bad = sys_stat("/etc", (fs_stat_t *)0);                 /* NULL must be rejected, not crash */
        sys_stats(&after);
        bool good = pid == 1 && fd == 3 && n == 6 && host[0] == 'm' && host[5] == 's' && cl == 0 && bad == SYS_EFAULT &&
                    after.total >= before.total + 5 && sys_process_list(NULL, 0) == 2;
        if (good) { ok_begin(); screen_puts("System calls: int 0x80, "); screen_put_dec(SYS_COUNT); screen_puts(" calls, trap self-test passed\n"); klogf(LOG_INFO, "boot", "System calls: int 0x80, %u calls, trap self-test passed", (uint32_t)SYS_COUNT); }
        else fail("System-call trap self-test");
    }
    {
        /* User manager: accounts and salted password hashes; from here on MiniFS acts for "nobody" until someone logs in.
         * Self-test through the real trap, without needing any plaintext password: logged out, wrong password, unknown user. */
        klog_mute(true);                                   /* the self-test fails logins on purpose: that must not look like an attack in the log */
        users_init();
        user_info_t first; int count = 0;
        while (sys_user_get((uint32_t)count, &first) == 0) count++;
        uint16_t fu, fg; fs_get_user(&fu, &fg);
        bool good = count == 4 && sys_getuid() == SYS_ENOUSER && sys_verify("student", "not-the-password") == SYS_EAUTH &&
                    sys_verify("nosuchuser", "whatever") == SYS_ENOUSER && sys_login("root", "wrong-password") == SYS_EAUTH &&
                    sys_user_add("intruder", "intruder1", 0) == SYS_EACCES && fu == UID_NOBODY && sys_getuid() == SYS_ENOUSER;
        if (good) { ok_begin(); screen_put_dec((uint32_t)count); screen_puts(" accounts, salted SHA-256 passwords, login required (self-test passed)\n"); klogf(LOG_INFO, "boot", "User manager: %u accounts, salted SHA-256 passwords, login required", (uint32_t)count); }
        else fail("User manager self-test");
        users_init();                                     /* start with a clean audit trail and counters: the self-test is not a user action */
        klog_mute(false);
    }
    {
        screen_puts("[ "); screen_set_color(VGA_LIGHT_GREEN, VGA_BLACK); screen_puts("OK");
        screen_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        screen_puts(" ] Process manager: idle=PID 0, shell=PID 1, round robin, slice ");
        screen_put_dec(sched_timeslice()); screen_puts(" ticks\n");
        klogf(LOG_INFO, "boot", "Process manager: idle=PID 0, shell=PID 1, round robin, slice %u ticks", (uint32_t)sched_timeslice());
    }

    /* Command registry: validate the 300-entry table, then attach handlers. */
    size_t bad;
    int v = command_validate(&bad);
    if (v != CMDV_OK) {
        fail("Command registry is invalid:");
        screen_puts("       "); screen_puts(command_validate_error(v));
        screen_puts(" (entry "); screen_put_dec((uint32_t)bad + 1); screen_puts(")\n");
    } else {
        int unbound = builtins_register();
        screen_puts("[ "); screen_set_color(VGA_LIGHT_GREEN, VGA_BLACK); screen_puts("OK");
        screen_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        screen_puts(" ] Command registry: "); screen_put_dec((uint32_t)command_count());
        screen_puts(" commands (L1 "); screen_put_dec((uint32_t)command_count_level(LEVEL_L1));
        screen_puts(", L2 "); screen_put_dec((uint32_t)command_count_level(LEVEL_L2));
        screen_puts(", L3 "); screen_put_dec((uint32_t)command_count_level(LEVEL_L3));
        screen_puts("), "); screen_put_dec((uint32_t)command_count_implemented());
        screen_puts(" implemented\n");
        klogf(LOG_INFO, "boot", "Command registry: %u commands, %u implemented", (uint32_t)command_count(), (uint32_t)command_count_implemented());
        if (unbound) fail("some built-in handlers failed to bind (see commands/shell_commands.c)");
    }

    ok("Shell starting");
    klog(LOG_INFO, "boot", "Boot complete");
    klog_boot_done();
    screen_puts("\nBoot complete. Type 'help'.\n\n");

    shell_run();   /* never returns */
}
