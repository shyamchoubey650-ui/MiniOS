/* Integration test: runs the REAL memory command handlers (commands/memory_commands.c)
 * through the real parser and registry, on top of the real PMM + heap.
 * Only the screen driver is replaced by a capture buffer. Run with: make test */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "include/shell.h"
#include "include/command.h"
#include "include/builtins.h"
#include "include/memory.h"
#include "include/filesystem.h"
#include "include/heap.h"
#include "include/screen.h"

/* ---- screen stubs: capture everything into `out` ---- */

/* ---- Phase 9: the commands now go through system calls, which need these ---- */
#include "include/interrupts.h"
#include "include/syscall.h"
#include "include/process.h"
#include "tests/low_stack.h"
void interrupt_register(uint32_t v, interrupt_handler_t h) { (void)v; (void)h; }
static char out[16384]; static size_t out_len;
static void out_reset(void) { out_len = 0; out[0] = 0; }
void screen_init(void) {}
void screen_clear(void) {}
void screen_set_color(uint8_t fg, uint8_t bg) { (void)fg; (void)bg; }
void screen_backspace(void) {}
void screen_putc(char c) { if (out_len + 1 < sizeof out) { out[out_len++] = c; out[out_len] = 0; } }
void screen_puts(const char *s) { while (*s) screen_putc(*s++); }
void screen_put_dec(uint32_t v) { char b[16]; snprintf(b, sizeof b, "%u", v); screen_puts(b); }
void screen_put_hex(uint32_t v) { char b[16]; snprintf(b, sizeof b, "0x%08X", v); screen_puts(b); }
uint32_t timer_ticks(void) { return 0; }
int keyboard_has_key(void) { return 0; }
int keyboard_getkey(void) { return 0; }
const char *exception_name(uint32_t v) { (void)v; return "exception"; }
/* registry stub used by builtins.h consumers: command_stub is not needed here */
int command_stub(const command_t *c, int argc, const char **argv) { (void)c; (void)argc; (void)argv; return 2; }

static int run, failed;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; \
    printf("  FAIL line %d: %s\n    output was: [%s]\n", __LINE__, #cond, out); } } while (0)

/* Parse like the shell does, look up the command, run it. */
static int sh(const char *line) {
    static char buf[SHELL_LINE_MAX * 2];
    command_line_t cl;
    out_reset();
    if (parse_command_line(line, buf, sizeof buf, &cl) != PARSE_OK) return -1;
    const command_t *c = command_find(cl.argv[0]);
    if (!c || !c->handler) return -2;
    return c->handler(cl.argc, cl.argv);
}
static bool has(const char *needle) { return strstr(out, needle) != NULL; }

static multiboot_mmap_entry_t map_buf[3];
static multiboot_info_t mbi;

static int real_main(void) {
    map_buf[0] = (multiboot_mmap_entry_t){ 20, 0, 0x9FC00, 1 };
    map_buf[1] = (multiboot_mmap_entry_t){ 20, 0xF0000, 0x10000, 2 };
    map_buf[2] = (multiboot_mmap_entry_t){ 20, 0x100000, 0x7EE0000, 1 };
    mbi.flags = MULTIBOOT_FLAG_MEM | MULTIBOOT_FLAG_MMAP;
    mbi.mmap_addr = (uint32_t)(uintptr_t)map_buf; mbi.mmap_length = sizeof map_buf;
    CHECK(memory_init(&mbi, 0x100000, 0x150000) == MEM_OK);
    fs_init(); process_init(); syscall_init();         /* alloc/dealloc are system calls and need a process */
    CHECK(memory_commands_register() == 0);                    /* every memory command exists in the registry */
    CHECK(command_find("mem") && command_find("mem")->handler);

    puts("memory commands (integration)");
    CHECK(sh("mem") == 0 && has("Total") && has("Free") && has("Heap"));
    CHECK(sh("free") == 0 && has("Physical") && has("Heap"));
    CHECK(sh("meminfo") == 0 && has("Kernel heap") && has("Not implemented: paging"));
    CHECK(sh("memmap") == 0 && has("available") && has("reserved"));
    CHECK(sh("pages") == 0 && has("MiB |") && has("R=reserved"));
    CHECK(sh("heap") == 0 && has("Integrity : OK"));
    CHECK(sh("memstat") == 0 && has("successful allocations"));
    CHECK(sh("memfrag") == 0 && has("0%"));
    CHECK(sh("blocks") == 0 && !has("alloc #"));              /* no allocations of the alloc command (file-system nodes etc. still show as "kernel") */

    /* alloc / dealloc lifecycle */
    CHECK(sh("alloc 100") == 0 && has("allocated #1: 100 bytes (block 112)"));
    CHECK(sh("alloc 0x40") == 0 && has("allocated #2: 64 bytes"));
    CHECK(sh("blocks") == 0 && has("alloc #1") && has("alloc #2") && has("asked 100"));
    CHECK(sh("mem") == 0 && has("allocated blocks") && sh("blocks") == 0 && has("alloc #1") && has("alloc #2"));
    CHECK(sh("dealloc 1") == 0 && has("freed #1"));
    CHECK(sh("dealloc 1") == 1 && has("allocation #1 does not exist"));          /* the spec's style of error */
    CHECK(sh("dealloc #2") == 0 && has("freed #2"));
    CHECK(sh("dealloc 999") == 1 && has("does not exist"));
    CHECK(sh("dealloc abc") == 1 && has("not a valid allocation number"));
    CHECK(sh("dealloc") == 1 && has("usage:"));
    CHECK(sh("blocks") == 0 && !has("alloc #"));              /* no allocations of the alloc command (file-system nodes etc. still show as "kernel") */

    /* invalid alloc input */
    CHECK(sh("alloc") == 1 && has("usage:"));
    CHECK(sh("alloc 0") == 1 && has("at least 1"));
    CHECK(sh("alloc -5") == 1 && has("must be a number"));
    CHECK(sh("alloc 12abc") == 1 && has("must be a number"));
    CHECK(sh("alloc 99999999999") == 1 && has("must be a number"));              /* > 32 bits */
    CHECK(sh("alloc 100000000") == 1 && has("64 MiB command limit"));
    CHECK(sh("alloc 8000000") == 1 && has("out of memory") && has("largest free block"));   /* bigger than the 4 MiB heap */
    heap_stats_t hs; heap_stats(&hs);
    CHECK(hs.failed_allocs >= 1);
    CHECK(sh("memstat") == 0 && !has("failed allocations     : 0"));

    /* memory exhaustion by filling the heap, then recovery */
    char line[32];
    CHECK(sh("alloc 3000000") == 0);                            /* 3 MB of the 4 MiB heap */
    uint32_t big_id = 0; { const char *p = strstr(out, "allocated #"); if (p) big_id = (uint32_t)strtoul(p + 11, NULL, 10); }
    CHECK(big_id != 0);
    CHECK(sh("alloc 3000000") == 1 && has("out of memory"));    /* second one cannot fit */
    CHECK(sh("heap") == 0 && has("Integrity : OK"));            /* a failed alloc must not damage the heap */
    snprintf(line, sizeof line, "dealloc %u", big_id);
    CHECK(sh(line) == 0);
    CHECK(sh("alloc 3000000") == 0);                            /* memory is reusable after the free */
    { const char *p = strstr(out, "allocated #"); big_id = (uint32_t)strtoul(p + 11, NULL, 10); }
    snprintf(line, sizeof line, "dealloc %u", big_id);
    CHECK(sh(line) == 0);
    CHECK(sh("heap") == 0 && has("Integrity : OK"));

    /* slot table limit (32) */
    int made = 0;
    for (int i = 0; i < 40; i++) { if (sh("alloc 16") == 0) made++; else break; }
    CHECK(made == 32);
    CHECK(sh("alloc 16") == 1 && has("slots are in use"));
    for (uint32_t id = 1; id < 1000 && made > 0; id++) { snprintf(line, sizeof line, "dealloc %u", id); if (sh(line) == 0) made--; }
    CHECK(made == 0);

    /* peek / poke / memfill / memdump */
    CHECK(sh("alloc 32") == 0);                                 /* the command prints the id and the address it got from the system call */
    uint32_t id = 0, addr = 0;
    { const char *p = strstr(out, "allocated #"); id = (uint32_t)strtoul(p + 11, NULL, 10); p = strstr(out, " at 0x"); addr = (uint32_t)strtoul(p + 4, NULL, 16); }
    CHECK(addr != 0);
    snprintf(line, sizeof line, "memfill #%u 0xAB", id);       CHECK(sh(line) == 0 && has("0xAB"));
    snprintf(line, sizeof line, "peek %u", addr);              CHECK(sh(line) == 0 && has("0x000000AB"));
    snprintf(line, sizeof line, "peek %u 4", addr);            CHECK(sh(line) == 0 && has("0xABABABAB"));
    snprintf(line, sizeof line, "poke %u 66", addr + 1);       CHECK(sh(line) == 0 && has("wrote 0x42"));
    snprintf(line, sizeof line, "peek #%u 2", id);             CHECK(sh(line) == 0 && has("0x000042AB"));
    snprintf(line, sizeof line, "poke %u 1", addr + 32);       CHECK(sh(line) == 1 && has("refusing to write"));   /* one byte past the block */
    snprintf(line, sizeof line, "poke %u 256", addr);          CHECK(sh(line) == 1 && has("0-255"));
    snprintf(line, sizeof line, "memfill %u 300", id);         CHECK(sh(line) == 1 && has("0-255"));
    snprintf(line, sizeof line, "memdump #%u", id);            CHECK(sh(line) == 0 && has("AB AB AB") && has("|"));
    snprintf(line, sizeof line, "memdump #%u 5", id);          CHECK(sh(line) == 0);
    CHECK(sh("poke 0x1000 1") == 1 && has("refusing to write"));                 /* low memory is readable but never writable */
    CHECK(sh("poke 0xB8000 65") == 1 && has("refusing"));                        /* cannot scribble on the VGA buffer */
    CHECK(sh("peek 0xFEE00000") == 1 && has("not readable"));                    /* APIC MMIO */
    CHECK(sh("peek 0xFFFFFFFF 4") == 1 && has("not readable"));                  /* wraps */
    CHECK(sh("peek 0x1000 3") == 1 && has("width must be"));
    CHECK(sh("peek zzz") == 1 && has("bad address"));
    CHECK(sh("peek #77") == 1 && has("no such allocation"));
    CHECK(sh("peek") == 1 && has("usage:"));
    CHECK(sh("memdump 0x1000 0") == 1 && has("1 to 256"));
    CHECK(sh("memdump 0x1000 257") == 1 && has("1 to 256"));
    CHECK(sh("memdump 0xFEE00000") == 1 && has("not readable"));
    CHECK(sh("memfill 99 1") == 1 && has("no such allocation"));
    snprintf(line, sizeof line, "dealloc %u", id);             CHECK(sh(line) == 0);
    snprintf(line, sizeof line, "peek #%u", id);               CHECK(sh(line) == 1 && has("no such allocation"));   /* handle is gone after free */

    /* memtest leaves the heap exactly as it found it, including with live allocations around it */
    CHECK(sh("alloc 5000") == 0);
    CHECK(sh("alloc 77") == 0);
    heap_stats_t before, after; heap_stats(&before);
    CHECK(sh("memtest") == 0 && has("PASS") && has("0 errors"));
    heap_stats(&after);
    CHECK(after.used_blocks == before.used_blocks && after.payload_used == before.payload_used && after.block_count == before.block_count);
    CHECK(after.alloc_count > before.alloc_count + 60);        /* it really did allocate */
    CHECK(heap_check(NULL) == HEAP_OK);
    CHECK(sh("memfrag") == 0 && has("fragmentation"));

    /* memtest on a nearly full heap reports failure honestly instead of claiming PASS */
    CHECK(sh("alloc 4000000") == 0 || sh("alloc 3900000") == 0 || sh("alloc 3500000") == 0);
    heap_stats(&hs);
    void *hog = hs.largest_free > 64 ? kmalloc(hs.largest_free) : NULL;     /* eat the rest of the heap */
    CHECK(sh("memtest") == 1 && has("FAIL"));
    if (hog) kfree(hog);

    /* corrupted heap: commands notice */
    heap_stats(&hs);
    {
        uint8_t *victim = kmalloc(64); ((uint8_t *)victim)[-32] ^= 0xFF;
        CHECK(sh("heap") == 1 && has("CORRUPT"));
        CHECK(sh("memtest") == 1 && has("already corrupt"));
        ((uint8_t *)victim)[-32] ^= 0xFF;                       /* repair so the later checks are meaningful */
        CHECK(sh("heap") == 0);
        kfree(victim);
    }

    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}

int main(void) { return run_on_low_stack(real_main); }
