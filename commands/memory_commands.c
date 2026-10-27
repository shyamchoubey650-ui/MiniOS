/* Memory commands (Phase 6): mem free meminfo memmap heap alloc dealloc memstat
 * memtest memfrag blocks memdump memfill pages peek poke.
 *
 * These call the kernel memory APIs directly. In Phase 9 they will move to the
 * system-call layer (sys_memory_alloc / sys_memory_free). */
#include "include/builtins.h"
#include "include/screen.h"
#include "include/memory.h"
#include "include/pmm.h"
#include "include/heap.h"
#include "include/numparse.h"
#include "include/syscall.h"
#include "include/string.h"

/* ---------- output helpers ---------- */
static void color(uint8_t fg) { screen_set_color(fg, VGA_BLACK); }

/* Formats a byte count as "4.0 MiB" / "639 KiB" / "12 B" into buf (>= 16 bytes). */
static void size_str(char *buf, uint32_t b) {
    char tmp[12]; size_t n = 0, w = 0;
    uint32_t whole, tenth = 0; const char *unit;
    if (b >= (1u << 20))      { whole = b >> 20; tenth = ((b & 0xFFFFFu) * 10) >> 20; unit = " MiB"; }
    else if (b >= 1024)       { whole = b >> 10; unit = " KiB"; }
    else                      { whole = b;       unit = " B"; }
    do { tmp[n++] = (char)('0' + whole % 10); whole /= 10; } while (whole);
    while (n) buf[w++] = tmp[--n];
    if (b >= (1u << 20)) { buf[w++] = '.'; buf[w++] = (char)('0' + tenth); }
    for (const char *u = unit; *u; u++) buf[w++] = *u;
    buf[w] = '\0';
}
static void put_size(uint32_t b) { char t[16]; size_str(t, b); screen_puts(t); }
static void put_size_pad(uint32_t b, size_t width) {
    char t[16]; size_str(t, b); screen_puts(t);
    for (size_t k = strlen(t); k < width; k++) screen_putc(' ');
}
static void put_kib_col(uint32_t bytes, size_t width) {
    uint32_t v = bytes >> 10; size_t d = 1;
    for (uint32_t t = v; t >= 10; t /= 10) d++;
    for (size_t k = d; k < width; k++) screen_putc(' ');
    screen_put_dec(v);
}
static void put_pad(const char *s, size_t w) { screen_puts(s); for (size_t k = strlen(s); k < w; k++) screen_putc(' '); }
static void put_hex_byte(uint8_t v) { static const char d[] = "0123456789ABCDEF"; screen_putc(d[v >> 4]); screen_putc(d[v & 15]); }
static void put_hex64(uint64_t v) {
    if ((v >> 32) == 0) { screen_put_hex((uint32_t)v); return; }
    screen_put_hex((uint32_t)(v >> 32));
    screen_puts(" ");                                      /* high word, then low word */
    screen_put_hex((uint32_t)v);
}
static int fail(const char *cmd, const char *msg) {
    color(VGA_LIGHT_RED); screen_puts("Error: "); color(VGA_LIGHT_GREY);
    screen_puts(cmd); screen_puts(": "); screen_puts(msg); screen_putc('\n');
    return 1;
}
static int usage(const char *text) { screen_puts("usage: "); screen_puts(text); screen_putc('\n'); return 1; }

/* ---------- allocations made by the `alloc` command ---------- */
#define ALLOC_SLOTS      32
#define ALLOC_MAX_BYTES  (64u * 1024u * 1024u)
typedef struct { void *ptr; size_t size; uint32_t id; } alloc_rec_t;
static alloc_rec_t recs[ALLOC_SLOTS];
static uint32_t    next_id = 1;

static alloc_rec_t *rec_by_id(uint32_t id) {
    for (int i = 0; i < ALLOC_SLOTS; i++) if (recs[i].ptr && recs[i].id == id) return &recs[i];
    return NULL;
}
static alloc_rec_t *rec_containing(uint32_t addr, uint32_t len) {
    for (int i = 0; i < ALLOC_SLOTS; i++) {
        if (!recs[i].ptr) continue;
        uint64_t lo = (uint32_t)(uintptr_t)recs[i].ptr, hi = lo + recs[i].size;
        if (addr >= lo && (uint64_t)addr + len <= hi) return &recs[i];
    }
    return NULL;
}
static const alloc_rec_t *rec_by_payload(uintptr_t a) {
    for (int i = 0; i < ALLOC_SLOTS; i++) if (recs[i].ptr && (uintptr_t)recs[i].ptr == a) return &recs[i];
    return NULL;
}
static bool parse_handle(const char *s, uint32_t *id) { if (s && *s == '#') s++; return parse_u32(s, id); }

/* Reads "<addr>" or "#<id>" into an address. */
static bool parse_address(const char *cmd, const char *arg, uint32_t *addr, const alloc_rec_t **rec_out) {
    if (rec_out) *rec_out = NULL;
    if (arg[0] == '#') {
        uint32_t id;
        if (!parse_handle(arg, &id)) { fail(cmd, "bad block number"); return false; }
        alloc_rec_t *r = rec_by_id(id);
        if (!r) { fail(cmd, "no such allocation (see 'blocks')"); return false; }
        *addr = (uint32_t)(uintptr_t)r->ptr;
        if (rec_out) *rec_out = r;
        return true;
    }
    if (!parse_u32(arg, addr)) { fail(cmd, "bad address (use decimal or 0x hex)"); return false; }
    return true;
}

/* ---------- mem / free / meminfo ---------- */
static int cmd_mem(int argc, const char **argv) {
    (void)argc; (void)argv;
    memory_summary_t m; memory_summary(&m);
    heap_stats_t h; heap_stats(&h);
    uint32_t used = m.ram_bytes - m.free_bytes;
    screen_puts("Memory Information\n");
    screen_puts("  Total : "); put_size(m.ram_bytes);  screen_puts("   (usable RAM per the bootloader map)\n");
    screen_puts("  Used  : "); put_size(used);         screen_puts("   (kernel + first MiB + heap region)\n");
    screen_puts("  Free  : "); put_size(m.free_bytes); screen_putc('\n');
    screen_puts("  Heap  : "); put_size((uint32_t)h.payload_used); screen_puts(" of ");
    put_size(m.heap_size); screen_puts(" in use, "); screen_put_dec((uint32_t)h.used_blocks); screen_puts(" allocated blocks\n");
    return 0;
}

static int cmd_free(int argc, const char **argv) {
    (void)argc; (void)argv;
    memory_summary_t m; memory_summary(&m);
    heap_stats_t h; heap_stats(&h);
    screen_puts("               total        used        free   (KiB)\n");
    screen_puts("Physical : "); put_kib_col(m.ram_bytes, 11); put_kib_col(m.ram_bytes - m.free_bytes, 12); put_kib_col(m.free_bytes, 12); screen_putc('\n');
    screen_puts("Heap     : "); put_kib_col((uint32_t)h.total_bytes, 11);
    put_kib_col((uint32_t)(h.payload_used + h.overhead_bytes), 12); put_kib_col((uint32_t)h.payload_free, 12); screen_putc('\n');
    screen_puts("(heap 'used' includes the 32-byte block headers; 'free' is usable payload)\n");
    return 0;
}

static int cmd_meminfo(int argc, const char **argv) {
    (void)argc; (void)argv;
    memory_summary_t m; memory_summary(&m);
    pmm_stats_t p = pmm_stats();
    heap_stats_t h; heap_stats(&h);
    screen_puts("Physical memory (page frames of 4 KiB)\n");
    screen_puts("  Map source      : "); screen_puts(memory_map_source()); screen_putc('\n');
    screen_puts("  RAM frames      : "); screen_put_dec(p.ram_frames); screen_puts("  ("); put_size(m.ram_bytes); screen_puts(")\n");
    screen_puts("  Free frames     : "); screen_put_dec(p.free_frames); screen_puts("  ("); put_size(m.free_bytes); screen_puts(")\n");
    screen_puts("  Allocated       : "); screen_put_dec(p.used_frames); screen_puts("  ("); put_size(m.allocated_bytes); screen_puts(", the heap)\n");
    screen_puts("  Reserved        : "); screen_put_dec(p.reserved_frames); screen_puts("  ("); put_size(m.reserved_bytes); screen_puts(")\n");
    screen_puts("  Kernel image    : "); put_size(m.kernel_bytes); screen_putc('\n');
    screen_puts("Kernel heap\n");
    screen_puts("  Region          : "); screen_put_hex(m.heap_base); screen_puts(" size "); put_size(m.heap_size); screen_putc('\n');
    screen_puts("  Blocks          : "); screen_put_dec((uint32_t)h.block_count); screen_puts(" ("); screen_put_dec((uint32_t)h.used_blocks);
    screen_puts(" used, "); screen_put_dec((uint32_t)h.free_blocks); screen_puts(" free)\n");
    screen_puts("  Payload used    : "); put_size((uint32_t)h.payload_used); screen_putc('\n');
    screen_puts("  Payload free    : "); put_size((uint32_t)h.payload_free); screen_puts(", largest "); put_size((uint32_t)h.largest_free); screen_putc('\n');
    screen_puts("  Header overhead : "); put_size((uint32_t)h.overhead_bytes); screen_puts(" ("); screen_put_dec((uint32_t)heap_header_size()); screen_puts(" B per block)\n");
    screen_puts("Not implemented: paging / virtual memory / memory protection (flat physical addressing)\n");
    return 0;
}

/* ---------- memmap ---------- */
static int cmd_memmap(int argc, const char **argv) {
    (void)argc; (void)argv;
    screen_puts("Memory map from: "); screen_puts(memory_map_source()); screen_putc('\n');
    screen_puts("  BASE        END         SIZE        TYPE\n");
    for (size_t i = 0; i < memory_region_count(); i++) {
        const mem_region_t *r = memory_region(i);
        uint64_t end = r->base + r->length - 1;
        screen_puts("  "); put_hex64(r->base); screen_puts("  "); put_hex64(end); screen_puts("  ");
        if (r->length >> 32) screen_puts(">4 GiB      "); else put_size_pad((uint32_t)r->length, 12);
        color(r->type == MULTIBOOT_MMAP_AVAILABLE ? VGA_LIGHT_GREEN : VGA_DARK_GREY);
        screen_puts(memory_region_type_name(r->type)); color(VGA_LIGHT_GREY); screen_putc('\n');
    }
    if (memory_regions_seen() > memory_region_count()) {
        screen_put_dec((uint32_t)(memory_regions_seen() - memory_region_count())); screen_puts(" more region(s) not stored\n");
    }
    return 0;
}

/* ---------- heap / blocks ---------- */
typedef struct { size_t shown, limit, index; bool only_used; } list_ctx_t;

static bool list_block(const heap_block_info_t *b, void *vctx) {
    list_ctx_t *c = vctx;
    c->index++;
    if (c->only_used && !b->used) return true;
    if (c->shown >= c->limit) { c->shown++; return true; }      /* count what we did not print */
    c->shown++;
    screen_puts("  "); put_pad(b->used ? "used" : "free", 6);
    screen_put_hex((uint32_t)b->addr); screen_puts("  ");
    screen_put_dec((uint32_t)b->size); screen_puts(" B");
    if (b->used) {
        screen_puts(" (asked "); screen_put_dec((uint32_t)b->requested); screen_puts(")");
        const alloc_rec_t *r = rec_by_payload(b->addr);
        if (r) { screen_puts("  alloc #"); screen_put_dec(r->id); }
        else screen_puts("  kernel");
    }
    screen_putc('\n');
    return true;
}

static void print_block_list(size_t limit, bool only_used) {
    list_ctx_t c = { 0, limit, 0, only_used };
    heap_walk(list_block, &c);
    if (c.shown > limit) { screen_puts("  ... "); screen_put_dec((uint32_t)(c.shown - limit)); screen_puts(" more\n"); }
    if (c.shown == 0) screen_puts("  (none)\n");
}

static int cmd_heap(int argc, const char **argv) {
    (void)argc; (void)argv;
    heap_stats_t h; heap_stats(&h);
    uintptr_t start; size_t size; heap_region(&start, &size);
    size_t bad;
    int chk = heap_check(&bad);
    screen_puts("Kernel heap: first-fit, "); screen_put_dec(HEAP_ALIGN); screen_puts("-byte alignment, split + coalesce\n");
    screen_puts("  Region    : "); screen_put_hex((uint32_t)start); screen_puts(" - "); screen_put_hex((uint32_t)(start + size));
    screen_puts("  ("); put_size((uint32_t)size); screen_puts(")\n");
    screen_puts("  Blocks    : "); screen_put_dec((uint32_t)h.block_count); screen_puts(" ("); screen_put_dec((uint32_t)h.used_blocks);
    screen_puts(" used, "); screen_put_dec((uint32_t)h.free_blocks); screen_puts(" free)\n");
    screen_puts("  Integrity : ");
    if (chk == HEAP_OK) { color(VGA_LIGHT_GREEN); screen_puts("OK"); color(VGA_LIGHT_GREY); }
    else { color(VGA_LIGHT_RED); screen_puts("CORRUPT at block "); screen_put_dec((uint32_t)bad); color(VGA_LIGHT_GREY); }
    screen_puts("\nBlock layout (address order):\n");
    print_block_list(12, false);
    return chk == HEAP_OK ? 0 : 1;
}

static int cmd_blocks(int argc, const char **argv) {
    (void)argc; (void)argv;
    screen_puts("Allocated blocks:\n");
    print_block_list(20, true);
    return 0;
}

/* ---------- alloc / dealloc ---------- */
static int cmd_alloc(int argc, const char **argv) {
    if (argc < 2) return usage("alloc <bytes>");
    uint32_t n;
    if (!parse_u32(argv[1], &n)) return fail("alloc", "size must be a number (decimal or 0x hex)");
    if (n == 0) return fail("alloc", "size must be at least 1 byte");
    if (n > ALLOC_MAX_BYTES) return fail("alloc", "size above the 64 MiB command limit");
    alloc_rec_t *slot = NULL;
    for (int i = 0; i < ALLOC_SLOTS && !slot; i++) if (!recs[i].ptr) slot = &recs[i];
    if (!slot) return fail("alloc", "all 32 allocation slots are in use (use dealloc)");

    void *p = NULL;
    int rc = sys_memory_alloc_err(n, &p);                   /* system call: tracked per process, freed if the process dies */
    if (rc == SYS_ELIMIT) return fail("alloc", "per-process allocation limit reached (use dealloc)");
    if (rc != 0) {
        heap_stats_t h; heap_stats(&h);
        color(VGA_LIGHT_RED); screen_puts("Error: "); color(VGA_LIGHT_GREY);
        screen_puts("out of memory: cannot allocate "); screen_put_dec(n); screen_puts(" bytes (largest free block: ");
        put_size((uint32_t)h.largest_free); screen_puts(")\n");
        return 1;
    }
    slot->ptr = p; slot->size = heap_block_size(p); slot->id = next_id++;
    screen_puts("allocated #"); screen_put_dec(slot->id); screen_puts(": "); screen_put_dec(n);
    screen_puts(" bytes (block "); screen_put_dec((uint32_t)slot->size); screen_puts(") at "); screen_put_hex((uint32_t)(uintptr_t)p); screen_putc('\n');
    return 0;
}

static int cmd_dealloc(int argc, const char **argv) {
    if (argc < 2) return usage("dealloc <number>   (numbers come from 'alloc' / 'blocks')");
    uint32_t id;
    if (!parse_handle(argv[1], &id)) return fail("dealloc", "not a valid allocation number");
    alloc_rec_t *r = rec_by_id(id);
    if (!r) {
        color(VGA_LIGHT_RED); screen_puts("Error: "); color(VGA_LIGHT_GREY);
        screen_puts("allocation #"); screen_put_dec(id); screen_puts(" does not exist\n");
        return 1;
    }
    int rc = sys_memory_free(r->ptr);
    if (rc != 0) return fail("dealloc", sys_strerror(rc));
    screen_puts("freed #"); screen_put_dec(id); screen_puts(" ("); screen_put_dec((uint32_t)r->size); screen_puts(" bytes)\n");
    r->ptr = NULL; r->size = 0;
    return 0;
}

/* ---------- memstat / memfrag ---------- */
static int cmd_memstat(int argc, const char **argv) {
    (void)argc; (void)argv;
    heap_stats_t h; heap_stats(&h);
    screen_puts("Heap counters\n");
    screen_puts("  successful allocations : "); screen_put_dec(h.alloc_count); screen_putc('\n');
    screen_puts("  successful frees       : "); screen_put_dec(h.free_count); screen_putc('\n');
    screen_puts("  failed allocations     : "); screen_put_dec(h.failed_allocs); screen_putc('\n');
    screen_puts("  live blocks            : "); screen_put_dec((uint32_t)h.used_blocks); screen_putc('\n');
    screen_puts("  peak payload in use    : "); put_size((uint32_t)h.peak_payload_used); screen_putc('\n');
    return 0;
}

static int cmd_memfrag(int argc, const char **argv) {
    (void)argc; (void)argv;
    heap_stats_t h; heap_stats(&h);
    uint32_t pct = 0;
    if (h.payload_free > 0) pct = 100 - (uint32_t)((h.largest_free * 100) / h.payload_free);
    screen_puts("Heap fragmentation estimate\n");
    screen_puts("  free payload  : "); put_size((uint32_t)h.payload_free); screen_putc('\n');
    screen_puts("  largest block : "); put_size((uint32_t)h.largest_free); screen_putc('\n');
    screen_puts("  free blocks   : "); screen_put_dec((uint32_t)h.free_blocks); screen_putc('\n');
    screen_puts("  fragmentation : "); screen_put_dec(pct); screen_puts("%  (0% = all free space in one piece)\n");
    return 0;
}

/* ---------- memtest: deterministic heap stress test ---------- */
#define TEST_BLOCKS 72
static uint32_t lcg_state;
static uint32_t lcg(void) { lcg_state = lcg_state * 1664525u + 1013904223u; return lcg_state >> 8; }

static bool pattern_ok(const uint8_t *p, size_t n, uint8_t v) { for (size_t i = 0; i < n; i++) if (p[i] != v) return false; return true; }

static int cmd_memtest(int argc, const char **argv) {
    (void)argc; (void)argv;
    uint8_t *blk[TEST_BLOCKS]; size_t len[TEST_BLOCKS]; uint8_t pat[TEST_BLOCKS];
    uint32_t allocs = 0, frees = 0, errors = 0;
    const char *why = NULL;
    heap_stats_t before, after;
    size_t bad;

    lcg_state = 2024;
    heap_stats(&before);
    if (heap_check(&bad) != HEAP_OK) return fail("memtest", "heap is already corrupt; refusing to run");
    screen_puts("memtest: stress-testing the kernel heap...\n");

    for (int i = 0; i < TEST_BLOCKS; i++) { blk[i] = NULL; len[i] = 0; pat[i] = 0; }

    /* phase 1: allocate 48 blocks of 16..2000 bytes and stamp each with its own byte */
    for (int i = 0; i < 48; i++) {
        len[i] = 16 + (lcg() % 1985); pat[i] = (uint8_t)(i * 37 + 1);
        blk[i] = kmalloc(len[i]);
        if (!blk[i]) { why = why ? why : "allocation failed (heap too full to run the test)"; len[i] = 0; continue; }
        allocs++;
        memset(blk[i], pat[i], len[i]);
    }
    for (int i = 0; i < 48; i++) if (blk[i] && !pattern_ok(blk[i], len[i], pat[i])) errors++;

    /* phase 2: free every odd block; survivors must be untouched; heap must stay consistent */
    for (int i = 1; i < 48; i += 2) if (blk[i]) { if (kfree(blk[i]) == HEAP_OK) frees++; else errors++; blk[i] = NULL; }
    for (int i = 0; i < 48; i += 2) if (blk[i] && !pattern_ok(blk[i], len[i], pat[i])) errors++;
    if (heap_check(&bad) != HEAP_OK) { errors++; why = why ? why : "heap metadata damaged after freeing"; }

    /* phase 3: allocate 24 more (they should reuse the holes), stamp and verify */
    for (int i = 48; i < TEST_BLOCKS; i++) {
        len[i] = 8 + (lcg() % 700); pat[i] = (uint8_t)(i * 53 + 7);
        blk[i] = kmalloc(len[i]);
        if (!blk[i]) { len[i] = 0; continue; }
        allocs++;
        memset(blk[i], pat[i], len[i]);
    }
    for (int i = 0; i < TEST_BLOCKS; i++) if (blk[i] && !pattern_ok(blk[i], len[i], pat[i])) errors++;

    /* phase 4: free everything left in a pseudo-random order */
    int order[TEST_BLOCKS];
    for (int i = 0; i < TEST_BLOCKS; i++) order[i] = i;
    for (int i = TEST_BLOCKS - 1; i > 0; i--) { int j = (int)(lcg() % (uint32_t)(i + 1)); int t = order[i]; order[i] = order[j]; order[j] = t; }
    for (int k = 0; k < TEST_BLOCKS; k++) {
        int i = order[k];
        if (!blk[i]) continue;
        if (!pattern_ok(blk[i], len[i], pat[i])) errors++;
        if (kfree(blk[i]) == HEAP_OK) frees++; else errors++;
        blk[i] = NULL;
    }

    /* the heap must look exactly like it did before the test */
    heap_stats(&after);
    if (heap_check(&bad) != HEAP_OK) { errors++; why = why ? why : "heap metadata damaged at the end"; }
    if (after.used_blocks != before.used_blocks || after.payload_used != before.payload_used ||
        after.block_count != before.block_count) { errors++; why = why ? why : "heap did not return to its starting layout (leak or missed merge)"; }

    screen_put_dec(allocs); screen_puts(" allocations, "); screen_put_dec(frees); screen_puts(" frees, ");
    screen_put_dec(errors); screen_puts(" errors -> ");
    if (errors == 0 && !why) { color(VGA_LIGHT_GREEN); screen_puts("PASS\n"); color(VGA_LIGHT_GREY); return 0; }
    color(VGA_LIGHT_RED); screen_puts("FAIL"); color(VGA_LIGHT_GREY);
    if (why) { screen_puts(": "); screen_puts(why); }
    screen_putc('\n');
    return 1;
}

/* ---------- memdump / memfill / peek / poke ---------- */
static int cmd_memdump(int argc, const char **argv) {
    if (argc < 2) return usage("memdump <address|#n> [length]   (length 1-256, default 64)");
    uint32_t addr, len = 64;
    const alloc_rec_t *r;
    if (!parse_address("memdump", argv[1], &addr, &r)) return 1;
    if (r && r->size < len) len = (uint32_t)r->size;
    if (argc >= 3 && (!parse_u32(argv[2], &len) || len == 0 || len > 256)) return fail("memdump", "length must be 1 to 256");
    if (!memory_range_readable(addr, len)) return fail("memdump", "range is not readable (only RAM and the first MiB are allowed)");

    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)addr;
    for (uint32_t off = 0; off < len; off += 16) {
        screen_put_hex(addr + off); screen_puts("  ");
        for (uint32_t k = 0; k < 16; k++) {
            if (off + k < len) { put_hex_byte(p[off + k]); screen_putc(' '); } else screen_puts("   ");
        }
        screen_putc('|');
        for (uint32_t k = 0; k < 16 && off + k < len; k++) { uint8_t c = p[off + k]; screen_putc(c >= 32 && c < 127 ? (char)c : '.'); }
        screen_puts("|\n");
    }
    return 0;
}

static int cmd_memfill(int argc, const char **argv) {
    if (argc < 3) return usage("memfill <number> <byte value 0-255>");
    uint32_t id, v;
    if (!parse_handle(argv[1], &id)) return fail("memfill", "not a valid allocation number");
    if (!parse_u32(argv[2], &v) || v > 255) return fail("memfill", "value must be 0-255");
    alloc_rec_t *r = rec_by_id(id);
    if (!r) return fail("memfill", "no such allocation (see 'blocks')");
    memset(r->ptr, (int)v, r->size);
    screen_puts("filled #"); screen_put_dec(id); screen_puts(" ("); screen_put_dec((uint32_t)r->size);
    screen_puts(" bytes) with 0x"); put_hex_byte((uint8_t)v); screen_putc('\n');
    return 0;
}

static int cmd_peek(int argc, const char **argv) {
    if (argc < 2) return usage("peek <address|#n> [1|2|4]");
    uint32_t addr, width = 1;
    if (!parse_address("peek", argv[1], &addr, NULL)) return 1;
    if (argc >= 3 && (!parse_u32(argv[2], &width) || (width != 1 && width != 2 && width != 4))) return fail("peek", "width must be 1, 2 or 4");
    if (!memory_range_readable(addr, width)) return fail("peek", "address is not readable (only RAM and the first MiB are allowed)");
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)addr;
    uint32_t v = 0;
    for (uint32_t i = 0; i < width; i++) v |= (uint32_t)p[i] << (8 * i);     /* little-endian, byte reads: no alignment faults */
    screen_put_hex(addr); screen_puts(": "); screen_put_hex(v); screen_puts("  ("); screen_put_dec(v); screen_puts(")\n");
    return 0;
}

static int cmd_poke(int argc, const char **argv) {
    if (argc < 3) return usage("poke <address|#n> <byte 0-255>   (only inside blocks from 'alloc')");
    uint32_t addr, v;
    if (!parse_address("poke", argv[1], &addr, NULL)) return 1;
    if (!parse_u32(argv[2], &v) || v > 255) return fail("poke", "value must be 0-255");
    if (!rec_containing(addr, 1)) {
        color(VGA_LIGHT_RED); screen_puts("Error: "); color(VGA_LIGHT_GREY);
        screen_puts("refusing to write "); screen_put_hex(addr); screen_puts(": not inside a block created by 'alloc' (poke is guarded)\n");
        return 1;
    }
    *(volatile uint8_t *)(uintptr_t)addr = (uint8_t)v;
    screen_puts("wrote 0x"); put_hex_byte((uint8_t)v); screen_puts(" to "); screen_put_hex(addr); screen_putc('\n');
    return 0;
}

/* ---------- pages: physical page-frame map ---------- */
static int cmd_pages(int argc, const char **argv) {
    (void)argc; (void)argv;
    pmm_stats_t p = pmm_stats();
    const uint32_t CHUNK = 256;                       /* frames per character = 1 MiB */
    screen_puts("Physical page frames (4 KiB each); one character = 1 MiB (256 frames)\n");
    for (uint32_t base = 0; base < p.highest_frame; base += CHUNK) {
        if (base % (64 * CHUNK) == 0) {
            if (base) screen_putc('\n');
            uint32_t mib = base / CHUNK; size_t digits = 1; for (uint32_t t = mib; t >= 10; t /= 10) digits++;
            for (size_t k = digits; k < 4; k++) screen_putc(' ');
            screen_put_dec(mib); screen_puts(" MiB |");
        }
        uint32_t n_none = 0, n_free = 0, n_used = 0, n_res = 0;
        for (uint32_t i = base; i < base + CHUNK; i++) {
            switch (pmm_frame_state(i)) {
                case PMM_FREE: n_free++; break;   case PMM_USED: n_used++; break;
                case PMM_RESERVED: n_res++; break; default: n_none++; break;
            }
        }
        char c = '+';
        if (n_none == CHUNK) c = '-'; else if (n_free == CHUNK) c = '.';
        else if (n_res == CHUNK) c = 'R'; else if (n_used == CHUNK) c = 'A';
        screen_putc(c);
    }
    screen_putc('\n');
    screen_puts(".=free  R=reserved (kernel, BIOS area)  A=allocated (heap)  +=mixed  -=no RAM\n");
    screen_puts("Runs of frames with the same state:\n");
    {
        uint32_t run_start = 0; pmm_state_t run_state = pmm_frame_state(0); uint32_t printed = 0;
        for (uint32_t i = 1; i <= p.highest_frame; i++) {
            pmm_state_t st = (i < p.highest_frame) ? pmm_frame_state(i) : (pmm_state_t)255;
            if (st == run_state) continue;
            if (printed < 12) {
                screen_puts("  "); screen_put_hex(run_start << PMM_FRAME_SHIFT); screen_puts(" - ");
                screen_put_hex((i << PMM_FRAME_SHIFT) - 1); screen_puts("  ");
                put_size_pad((i - run_start) << PMM_FRAME_SHIFT, 10);
                screen_puts(run_state == PMM_FREE ? "free" : run_state == PMM_USED ? "allocated (heap)" :
                            run_state == PMM_RESERVED ? "reserved" : "not RAM");
                screen_putc('\n');
            }
            printed++; run_start = i; run_state = st;
        }
        if (printed > 12) { screen_put_dec(printed - 12); screen_puts(" more runs\n"); }
    }
    screen_puts("Frames: "); screen_put_dec(p.ram_frames); screen_puts(" RAM = "); screen_put_dec(p.free_frames); screen_puts(" free + ");
    screen_put_dec(p.used_frames); screen_puts(" allocated + "); screen_put_dec(p.reserved_frames); screen_puts(" reserved\n");
    return 0;
}

/* ---------- registration ---------- */
static const struct { const char *name; cmd_handler_t fn; } bindings[] = {
    { "mem",      cmd_mem      },
    { "free",     cmd_free     },
    { "meminfo",  cmd_meminfo  },
    { "memmap",   cmd_memmap   },
    { "heap",     cmd_heap     },
    { "alloc",    cmd_alloc    },
    { "dealloc",  cmd_dealloc  },
    { "memstat",  cmd_memstat  },
    { "memtest",  cmd_memtest  },
    { "memfrag",  cmd_memfrag  },
    { "blocks",   cmd_blocks   },
    { "memdump",  cmd_memdump  },
    { "memfill",  cmd_memfill  },
    { "pages",    cmd_pages    },
    { "peek",     cmd_peek     },
    { "poke",     cmd_poke     },
};

int memory_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
