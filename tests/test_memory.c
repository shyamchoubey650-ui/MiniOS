/* Host-side tests for the Phase 6 memory subsystem: numparse, PMM, heap, memory_init.
 * They compile the REAL kernel sources. Run with: make test
 * (memory.c is built with -DMEMORY_HOST_TEST and -no-pie so fake Multiboot data
 *  can live at 32-bit addresses.) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "include/numparse.h"
#include "include/pmm.h"
#include "include/heap.h"
#include "include/memory.h"

static int run, failed;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; \
    printf("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

/* ---------------- numparse ---------------- */
static void test_numparse(void) {
    uint32_t v = 7;
    puts("numparse");
    CHECK(parse_u32("0", &v) && v == 0);
    CHECK(parse_u32("123", &v) && v == 123);
    CHECK(parse_u32("0x1F", &v) && v == 31);
    CHECK(parse_u32("0Xff", &v) && v == 255);
    CHECK(parse_u32("4294967295", &v) && v == 4294967295u);
    CHECK(parse_u32("0xFFFFFFFF", &v) && v == 0xFFFFFFFFu);
    v = 99;
    CHECK(!parse_u32("4294967296", &v) && v == 99);          /* overflow, *out untouched */
    CHECK(!parse_u32("0x100000000", &v));
    CHECK(!parse_u32("", &v));
    CHECK(!parse_u32("0x", &v));
    CHECK(!parse_u32("12a", &v));
    CHECK(!parse_u32("-1", &v));
    CHECK(!parse_u32("+1", &v));
    CHECK(!parse_u32(" 5", &v));
    CHECK(!parse_u32("0xG", &v));
    CHECK(!parse_u32(NULL, &v));
    CHECK(!parse_u32("5", NULL));
    CHECK(!parse_u32("1f", &v));                              /* hex digits need the 0x prefix */
}

/* ---------------- PMM ---------------- */
static void test_pmm(void) {
    uint32_t a, b;
    pmm_stats_t s;
    puts("pmm");
    pmm_init();
    s = pmm_stats();
    CHECK(s.ram_frames == 0 && s.free_frames == 0);
    CHECK(pmm_alloc(1, &a) == PMM_ERR_NO_MEMORY);            /* nothing to give */

    pmm_add_region(0x100000, 16u << 20);                      /* 16 MiB = 4096 frames */
    s = pmm_stats();
    CHECK(s.ram_frames == 4096 && s.free_frames == 4096);
    pmm_add_region(0x100000, 16u << 20);                      /* adding twice must not double count */
    CHECK(pmm_stats().ram_frames == 4096);

    pmm_reserve(0x100000, 0x1001);                            /* 4097 bytes touch 2 frames */
    s = pmm_stats();
    CHECK(s.reserved_frames == 2 && s.free_frames == 4094);
    CHECK(s.ram_frames == s.free_frames + s.used_frames + s.reserved_frames);
    CHECK(pmm_frame_state(0x100) == PMM_RESERVED && pmm_frame_state(0x102) == PMM_FREE);

    CHECK(pmm_alloc(1, &a) == PMM_OK && a == 0x102000);       /* lowest free frame, first-fit */
    CHECK(pmm_frame_state(a >> 12) == PMM_USED);
    CHECK(pmm_alloc(0, &b) == PMM_ERR_ARG);
    CHECK(pmm_alloc(1, NULL) == PMM_ERR_ARG);
    CHECK(pmm_alloc(5000, &b) == PMM_ERR_NO_MEMORY);          /* more than exists */
    CHECK(pmm_stats().free_frames == 4093);                   /* failed alloc changed nothing */

    CHECK(pmm_free(a, 1) == PMM_OK);
    CHECK(pmm_free(a, 1) == PMM_ERR_NOT_ALLOCATED);           /* double free */
    CHECK(pmm_free(a + 1, 1) == PMM_ERR_ARG);                 /* misaligned */
    CHECK(pmm_free(0x100000, 1) == PMM_ERR_NOT_ALLOCATED);    /* reserved frames are not freeable */
    CHECK(pmm_free(0x0, 1) == PMM_ERR_NOT_ALLOCATED);         /* non-RAM */
    CHECK(pmm_free(0xFFFFF000u, 1) == PMM_ERR_RANGE);
    CHECK(pmm_free(a, 0) == PMM_ERR_ARG);

    /* all-or-nothing free */
    uint32_t r;
    CHECK(pmm_alloc(3, &r) == PMM_OK);
    CHECK(pmm_free(r, 4) == PMM_ERR_NOT_ALLOCATED);           /* 4th frame is not ours */
    CHECK(pmm_frame_state(r >> 12) == PMM_USED && pmm_frame_state((r >> 12) + 2) == PMM_USED);
    CHECK(pmm_free(r, 3) == PMM_OK);

    /* fragmentation: plenty of free frames but no contiguous run */
    pmm_init();
    pmm_add_region(0x200000, 8 * PMM_FRAME_SIZE);             /* 8 frames */
    uint32_t f[8];
    for (int i = 0; i < 8; i++) CHECK(pmm_alloc(1, &f[i]) == PMM_OK);
    for (int i = 0; i < 8; i += 2) CHECK(pmm_free(f[i], 1) == PMM_OK);   /* free every other frame */
    CHECK(pmm_stats().free_frames == 4);
    CHECK(pmm_alloc(2, &a) == PMM_ERR_NO_MEMORY);             /* 4 free, but never 2 in a row */
    CHECK(pmm_alloc(1, &a) == PMM_OK);

    /* odd regions */
    pmm_init();
    pmm_add_region(0x2001, 0x3000);                           /* frames 3 and 4 only fully inside */
    CHECK(pmm_stats().ram_frames == 2 && pmm_frame_state(3) == PMM_FREE && pmm_frame_state(2) == PMM_NONRAM);
    pmm_add_region(0x5000, 0);                                /* empty */
    pmm_add_region(0xFFFFFFFFFFFFF000ull, 0x2000);            /* wraps / beyond limit: ignored */
    CHECK(pmm_stats().ram_frames == 2);
    pmm_init();
    pmm_add_region(0x3FF00000ull, 0x10000000ull);             /* crosses the 1 GiB limit: clamped */
    CHECK(pmm_stats().ram_frames == 256);
    pmm_add_region(0x1000, 0x1000);
    pmm_reserve(0x1000, 0xFFFFFFFFFFFFFFFFull);               /* length wraps: saturates, no crash */
    CHECK(pmm_stats().reserved_frames >= 1);
    CHECK(strlen(pmm_strerror(PMM_ERR_RANGE)) > 0);
}

/* ---------------- heap ---------------- */
static uint8_t arena[64 * 1024] __attribute__((aligned(16)));
static const size_t HDR = 32;

static void check_invariant(void) {
    heap_stats_t s; heap_stats(&s);
    CHECK(s.total_bytes == s.payload_used + s.payload_free + s.overhead_bytes);
    CHECK(s.block_count == s.used_blocks + s.free_blocks);
    CHECK(heap_check(NULL) == HEAP_OK);
}

static size_t walk_count; static size_t walk_payload;
static bool walk_cb(const heap_block_info_t *b, void *ctx) { (void)ctx; walk_count++; walk_payload += b->size; return true; }
static bool walk_stop(const heap_block_info_t *b, void *ctx) { (void)b; (*(int *)ctx)++; return false; }

static uint32_t rng = 12345;
static uint32_t next_rand(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void test_heap(void) {
    puts("heap");
    CHECK(heap_alloc(10) == NULL);                            /* before init */
    CHECK(heap_free((void *)arena) == HEAP_ERR_NOT_INIT);
    CHECK(heap_init(NULL, 100) == HEAP_ERR_NULL);
    CHECK(heap_init(arena, 16) == HEAP_ERR_TOO_SMALL);
    CHECK(heap_init(arena, 3) == HEAP_ERR_TOO_SMALL);
    CHECK(heap_header_size() == HDR);

    CHECK(heap_init(arena, sizeof arena) == HEAP_OK);
    heap_stats_t s; heap_stats(&s);
    CHECK(s.total_bytes == sizeof arena && s.free_blocks == 1 && s.used_blocks == 0);
    CHECK(s.largest_free == sizeof arena - HDR);
    check_invariant();

    /* basic allocation properties */
    CHECK(heap_alloc(0) == NULL);
    void *a = heap_alloc(1), *b = heap_alloc(17), *c = heap_alloc(100);
    CHECK(a && b && c);
    CHECK(((uintptr_t)a & 15) == 0 && ((uintptr_t)b & 15) == 0 && ((uintptr_t)c & 15) == 0);
    CHECK(heap_block_size(a) == 16 && heap_block_size(b) == 32 && heap_block_size(c) == 112);
    CHECK((uint8_t *)b >= (uint8_t *)a + 16 + HDR);           /* blocks never overlap */
    memset(a, 0xAA, 1); memset(b, 0xBB, 17); memset(c, 0xCC, 100);
    CHECK(((uint8_t *)a)[0] == 0xAA && ((uint8_t *)b)[16] == 0xBB && ((uint8_t *)c)[99] == 0xCC);
    CHECK(heap_owns(a) && !heap_owns(&s));
    check_invariant();

    /* invalid frees */
    CHECK(heap_free(NULL) == HEAP_ERR_NULL);
    CHECK(heap_free((uint8_t *)a + 1) == HEAP_ERR_BAD_POINTER);
    CHECK(heap_free((uint8_t *)a + 16) == HEAP_ERR_BAD_POINTER);   /* inside heap, aligned, not a block */
    CHECK(heap_free(&s) == HEAP_ERR_RANGE);
    CHECK(heap_free(arena) == HEAP_ERR_RANGE);                /* the first header is not a payload */
    CHECK(heap_block_size(&s) == 0 && heap_block_size(NULL) == 0);
    CHECK(heap_free(b) == HEAP_OK);
    CHECK(heap_free(b) == HEAP_ERR_DOUBLE_FREE);
    CHECK(heap_free(a) == HEAP_OK);
    CHECK(heap_free(c) == HEAP_OK);
    heap_stats(&s);
    CHECK(s.used_blocks == 0 && s.free_blocks == 1);          /* everything merged back */
    check_invariant();

    /* oversize and exhaustion */
    uint32_t failed_before = (heap_stats(&s), s.failed_allocs);
    CHECK(heap_alloc((size_t)-1) == NULL);
    CHECK(heap_alloc(sizeof arena) == NULL);
    CHECK(heap_alloc(sizeof arena - HDR + 1) == NULL);
    heap_stats(&s);
    CHECK(s.failed_allocs == failed_before + 3);
    void *whole = heap_alloc(sizeof arena - HDR);             /* exactly everything */
    CHECK(whole != NULL);
    heap_stats(&s);
    CHECK(s.free_blocks == 0 && s.used_blocks == 1 && s.payload_free == 0);
    CHECK(heap_alloc(1) == NULL);                             /* full */
    CHECK(heap_free(whole) == HEAP_OK);
    check_invariant();

    void *blocks[200]; int n = 0;
    while (n < 200 && (blocks[n] = heap_alloc(1000)) != NULL) n++;
    CHECK(n > 10 && n < 200);                                 /* ran out before 200 */
    CHECK(heap_alloc(1000) == NULL);
    check_invariant();
    for (int i = 0; i < n; i++) CHECK(heap_free(blocks[i]) == HEAP_OK);
    heap_stats(&s);
    CHECK(s.used_blocks == 0 && s.free_blocks == 1 && s.largest_free == sizeof arena - HDR);

    /* coalescing in every order */
    int orders[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    for (int o = 0; o < 6; o++) {
        void *p[3] = { heap_alloc(64), heap_alloc(64), heap_alloc(64) };
        for (int k = 0; k < 3; k++) CHECK(heap_free(p[orders[o][k]]) == HEAP_OK);
        heap_stats(&s);
        CHECK(s.free_blocks == 1 && s.used_blocks == 0);
    }

    /* first-fit reuses the freed hole */
    void *x = heap_alloc(64), *y = heap_alloc(64);
    CHECK(heap_free(x) == HEAP_OK);
    void *z = heap_alloc(64);
    CHECK(z == x);
    CHECK(heap_free(y) == HEAP_OK && heap_free(z) == HEAP_OK);

    /* splitting rule: remainder smaller than header+16 stays inside the block */
    void *big = heap_alloc(sizeof arena - HDR - 16 - HDR + 16);   /* leaves < HDR+16 spare */
    CHECK(big != NULL);
    heap_stats(&s);
    CHECK(s.free_blocks == 0 && s.used_blocks == 1);           /* no split: spare bytes stay in the block */
    CHECK(heap_block_size(big) == sizeof arena - HDR);
    CHECK(heap_free(big) == HEAP_OK);
    check_invariant();

    /* calloc */
    void *g = heap_alloc(256);
    memset(g, 0xFF, 256);
    heap_free(g);
    uint8_t *z16 = heap_calloc(16, 16);
    int zero = 1; for (int i = 0; i < 256; i++) if (z16[i]) zero = 0;
    CHECK(z16 && zero);
    CHECK(heap_calloc((size_t)-1 / 2, 4) == NULL);            /* count*size overflows */
    CHECK(heap_calloc(0, 10) == NULL);
    heap_free(z16);

    /* use-after-free becomes visible through poisoning */
    uint8_t *u = heap_alloc(64); memset(u, 1, 64);
    void *keep = heap_alloc(64);                              /* prevent merging with the tail */
    heap_free(u);
    CHECK(u[5] == 0xDD);
    heap_free(keep);

    /* walk */
    heap_init(arena, sizeof arena);
    void *w1 = heap_alloc(100), *w2 = heap_alloc(200); (void)w2;
    walk_count = 0; walk_payload = 0;
    heap_walk(walk_cb, NULL);
    heap_stats(&s);
    CHECK(walk_count == s.block_count && walk_payload == s.payload_used + s.payload_free);
    int stops = 0; heap_walk(walk_stop, &stops); CHECK(stops == 1);
    CHECK(s.peak_payload_used == s.payload_used);
    heap_free(w1);
    heap_stats(&s);
    CHECK(s.peak_payload_used > s.payload_used);              /* peak remembers the high mark */

    /* misaligned base: heap aligns itself */
    heap_init(arena + 3, 4096);
    heap_stats(&s);
    CHECK(s.total_bytes <= 4096 - 13 && (s.total_bytes & 15) == 0);
    void *m = heap_alloc(10);
    CHECK(m && ((uintptr_t)m & 15) == 0);
    heap_init(arena, sizeof arena);

    /* corruption is detected, not ignored */
    uint8_t *cp = heap_alloc(64);
    CHECK(heap_check(NULL) == HEAP_OK);
    cp[-HDR] ^= 0xFF;                                         /* damage the header's magic */
    size_t bad = 99;
    CHECK(heap_check(&bad) == HEAP_ERR_CORRUPT && bad == 0);
    CHECK(heap_free(cp) == HEAP_ERR_BAD_POINTER);             /* refuses to touch it */
    heap_init(arena, sizeof arena);

    /* forged header inside a payload is rejected */
    uint8_t *host = heap_alloc(256);
    memcpy(host + 64, host - HDR, HDR);                       /* copy of a valid header at another address */
    CHECK(heap_free(host + 64 + HDR) != HEAP_OK);
    heap_init(arena, sizeof arena);

    /* randomized stress with pattern verification (deterministic seed) */
    enum { SLOTS = 64 };
    uint8_t *slot[SLOTS] = {0}; size_t len[SLOTS] = {0}; uint8_t pat[SLOTS] = {0};
    int ops_ok = 1, pattern_ok = 1;
    for (int op = 0; op < 4000; op++) {
        uint32_t r = next_rand(); int i = (int)(r % SLOTS);
        if (slot[i]) {
            for (size_t k = 0; k < len[i]; k++) if (slot[i][k] != pat[i]) pattern_ok = 0;
            if (heap_free(slot[i]) != HEAP_OK) ops_ok = 0;
            slot[i] = NULL;
        } else {
            len[i] = 1 + (next_rand() % 900); pat[i] = (uint8_t)(op * 7 + 1);
            slot[i] = heap_alloc(len[i]);
            if (slot[i]) memset(slot[i], pat[i], len[i]);
        }
        if (op % 100 == 0 && heap_check(NULL) != HEAP_OK) ops_ok = 0;
    }
    CHECK(ops_ok && pattern_ok);
    check_invariant();
    for (int i = 0; i < SLOTS; i++) if (slot[i]) CHECK(heap_free(slot[i]) == HEAP_OK);
    heap_stats(&s);
    CHECK(s.used_blocks == 0 && s.free_blocks == 1);          /* no leaks, fully coalesced */
    CHECK(strlen(heap_strerror(HEAP_ERR_DOUBLE_FREE)) > 0);
}

/* ---------------- memory_init (fake Multiboot data) ---------------- */
static multiboot_mmap_entry_t map_buf[8];       /* static + -no-pie => 32-bit address */
static multiboot_info_t       fake_mbi;

static void set_entry(int i, uint64_t base, uint64_t len, uint32_t type) {
    map_buf[i].size = 20; map_buf[i].addr = base; map_buf[i].len = len; map_buf[i].type = type;
}
static void use_map(int n) {
    memset(&fake_mbi, 0, sizeof fake_mbi);
    fake_mbi.flags = MULTIBOOT_FLAG_MEM | MULTIBOOT_FLAG_MMAP;
    fake_mbi.mmap_addr = (uint32_t)(uintptr_t)map_buf;
    fake_mbi.mmap_length = (uint32_t)(n * sizeof(map_buf[0]));
}

static void test_memory_init(void) {
    puts("memory_init");
    memory_summary_t m;

    CHECK((uintptr_t)map_buf < 0xFFFFFFFFu);                  /* test setup sanity */
    CHECK(memory_init(NULL, 0x100000, 0x200000) == MEM_ERR_NO_INFO);
    memset(&fake_mbi, 0, sizeof fake_mbi);
    CHECK(memory_init(&fake_mbi, 0x100000, 0x200000) == MEM_ERR_NO_INFO);   /* no flags at all */

    /* typical QEMU map: 639 KiB low RAM, hole, 127 MiB above 1 MiB */
    set_entry(0, 0, 0x9FC00, 1);
    set_entry(1, 0x9FC00, 0x400, 2);
    set_entry(2, 0xF0000, 0x10000, 2);
    set_entry(3, 0x100000, 0x7EE0000, 1);
    set_entry(4, 0x7FE0000, 0x20000, 2);
    set_entry(5, 0xFFFC0000ull, 0x40000, 2);
    use_map(6);
    CHECK(memory_init(&fake_mbi, 0x100000, 0x108000) == MEM_OK);
    memory_summary(&m);
    CHECK(m.ram_bytes == 0x9F000 + 0x7EE0000);                /* whole frames of "available" regions only */
    CHECK(m.kernel_bytes == 0x8000);
    CHECK(m.heap_size == HEAP_DEFAULT_BYTES);
    CHECK(m.heap_base >= 0x108000);                           /* heap does not overlap kernel or low memory */
    CHECK(m.allocated_bytes == m.heap_size);
    CHECK(m.ram_bytes == m.free_bytes + m.allocated_bytes + m.reserved_bytes);
    CHECK(m.reserved_bytes >= 0x9F000 + 0x8000);              /* low memory + kernel image */
    CHECK(memory_region_count() == 6 && memory_regions_seen() == 6);
    CHECK(strcmp(memory_map_source(), "multiboot memory map") == 0);
    CHECK(memory_region(0)->type == 1 && memory_region(6) == NULL);

    /* memory is usable through kmalloc */
    char *p = kmalloc(100);
    CHECK(p != NULL);
    memset(p, 1, 100);
    CHECK(kfree(p) == HEAP_OK && kfree(p) == HEAP_ERR_DOUBLE_FREE);
    CHECK(kcalloc(4, 4) != NULL);

    /* readable-range guard */
    CHECK(memory_range_readable(0xB8000, 4000));              /* VGA text buffer */
    CHECK(memory_range_readable(0x0, 1));
    CHECK(memory_range_readable(0x200000, 64));               /* inside available RAM */
    CHECK(!memory_range_readable(0x200000, 0));               /* zero length */
    CHECK(!memory_range_readable(0xFEE00000u, 4));            /* APIC MMIO: not RAM */
    CHECK(!memory_range_readable(0xFFFFFFF0u, 0x100));        /* wraps past 4 GiB */
    CHECK(!memory_range_readable(0x7FE0000, 4));              /* reserved region at top of RAM */
    CHECK(!memory_range_readable(0x7FDFFF0, 0x20));           /* straddles available -> reserved */
    CHECK(memory_range_readable(0x7FDFFFF, 1));               /* very last byte of available RAM */
    CHECK(!memory_range_readable(0x7FDFFFF, 2));              /* one byte too many */
    CHECK(memory_range_readable(0xFFFF0, 0x10));              /* exactly the end of the first MiB */
    CHECK(!memory_range_readable(0xFFFF0, 0x11));             /* crosses 1 MiB into a range that starts above it */
    CHECK(memory_range_readable(0x100000, 16));

    /* fallback when there is no map: use mem_lower / mem_upper */
    memset(&fake_mbi, 0, sizeof fake_mbi);
    fake_mbi.flags = MULTIBOOT_FLAG_MEM; fake_mbi.mem_lower = 639; fake_mbi.mem_upper = 31 * 1024;
    CHECK(memory_init(&fake_mbi, 0x100000, 0x110000) == MEM_OK);
    CHECK(strcmp(memory_map_source(), "mem_lower/mem_upper (no map)") == 0);
    memory_summary(&m);
    CHECK(m.ram_bytes == 0x9F000 + 31u * 1024 * 1024);

    /* tiny machine: heap shrinks to half of free memory, then refuses if too small */
    set_entry(0, 0x100000, 0x100000, 1);                      /* 1 MiB of RAM above the first MiB */
    use_map(1);
    CHECK(memory_init(&fake_mbi, 0x100000, 0x110000) == MEM_OK);
    memory_summary(&m);
    CHECK(m.heap_size < HEAP_DEFAULT_BYTES && m.heap_size >= HEAP_MIN_BYTES);
    set_entry(0, 0x100000, 0x20000, 1);                       /* 128 KiB: half is below the 64 KiB minimum */
    use_map(1);
    CHECK(memory_init(&fake_mbi, 0x100000, 0x118000) == MEM_ERR_NO_HEAP);
    set_entry(0, 0x100000, 0x200000, 2);                      /* only reserved memory */
    use_map(1);
    CHECK(memory_init(&fake_mbi, 0x100000, 0x110000) == MEM_ERR_NO_RAM);

    /* malformed map entry stops parsing but keeps earlier regions */
    set_entry(0, 0x100000, 0x800000, 1);
    map_buf[1].size = 3;
    use_map(2);
    CHECK(memory_init(&fake_mbi, 0x100000, 0x110000) == MEM_OK && memory_region_count() == 1);

    /* more regions than we store: counted but not stored */
    static multiboot_mmap_entry_t many[20];
    for (int i = 0; i < 20; i++) { many[i].size = 20; many[i].addr = 0x100000 + (uint64_t)i * 0x400000; many[i].len = 0x400000; many[i].type = 1; }
    memset(&fake_mbi, 0, sizeof fake_mbi);
    fake_mbi.flags = MULTIBOOT_FLAG_MMAP; fake_mbi.mmap_addr = (uint32_t)(uintptr_t)many; fake_mbi.mmap_length = sizeof many;
    CHECK(memory_init(&fake_mbi, 0x100000, 0x110000) == MEM_OK);
    CHECK(memory_regions_seen() == 20 && memory_region_count() == MEM_MAX_REGIONS);
    CHECK(strlen(memory_strerror(MEM_ERR_NO_HEAP)) > 0 && strlen(memory_region_type_name(3)) > 0);
}

int main(void) {
    test_numparse(); test_pmm(); test_heap(); test_memory_init();
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
