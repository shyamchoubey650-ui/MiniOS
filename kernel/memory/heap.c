/* First-fit heap with block splitting and coalescing.
 *
 * Layout:  [hdr|payload][hdr|payload]...   (always in address order)
 * Safety:  the header magic mixes a constant with the block's own address, so a
 *          copied/forged header at another address does not validate. free() checks
 *          range, alignment, magic, and the list links before touching anything.
 * Freed payloads are overwritten with HEAP_POISON to make use-after-free visible. */
#include "include/heap.h"
#include "include/string.h"

#define HEAP_POISON      0xDD
#define MAGIC_USED_BASE  0xA110C8EDu
#define MAGIC_FREE_BASE  0xF4EEB10Cu

typedef struct block {
    uint32_t      magic;        /* encodes state (used/free) and address */
    uint32_t      requested;    /* what the caller asked for (clamped to 32 bits) */
    size_t        size;         /* payload bytes */
    struct block *next;         /* next block in ADDRESS order */
    struct block *prev;
} __attribute__((aligned(16))) block_t;

#define HDR ((size_t)sizeof(block_t))
_Static_assert(sizeof(block_t) % 16 == 0, "block header must keep payloads 16-byte aligned");

static block_t *first;
static uintptr_t heap_lo, heap_hi;      /* [heap_lo, heap_hi) */
static size_t   used_payload, peak_payload;
static uint32_t n_alloc, n_free, n_failed;

static uint32_t magic_used(const block_t *b) { return MAGIC_USED_BASE ^ (uint32_t)(uintptr_t)b; }
static uint32_t magic_free(const block_t *b) { return MAGIC_FREE_BASE ^ (uint32_t)(uintptr_t)b; }
static bool is_used(const block_t *b) { return b->magic == magic_used(b); }
static bool is_free(const block_t *b) { return b->magic == magic_free(b); }
static void *payload(block_t *b) { return (uint8_t *)b + HDR; }
static size_t align_up(size_t n) { return (n + (HEAP_ALIGN - 1)) & ~(size_t)(HEAP_ALIGN - 1); }

int heap_init(void *base, size_t size) {
    if (!base) return HEAP_ERR_NULL;
    uintptr_t lo = ((uintptr_t)base + (HEAP_ALIGN - 1)) & ~(uintptr_t)(HEAP_ALIGN - 1);
    size_t lost = (size_t)(lo - (uintptr_t)base);
    if (size < lost) return HEAP_ERR_TOO_SMALL;
    size = (size - lost) & ~(size_t)(HEAP_ALIGN - 1);
    if (size < HDR + HEAP_ALIGN) return HEAP_ERR_TOO_SMALL;

    heap_lo = lo; heap_hi = lo + size;
    first = (block_t *)lo;
    first->size = size - HDR;
    first->requested = 0;
    first->next = first->prev = NULL;
    first->magic = magic_free(first);
    used_payload = peak_payload = 0;
    n_alloc = n_free = n_failed = 0;
    return HEAP_OK;
}

void *heap_alloc(size_t size) {
    if (!first || size == 0) return NULL;
    if (size > heap_hi - heap_lo) { n_failed++; return NULL; }     /* also prevents overflow below */
    size_t need = align_up(size);

    for (block_t *b = first; b; b = b->next) {
        if (!is_free(b) || b->size < need) continue;
        if (b->size >= need + HDR + HEAP_ALIGN) {                  /* split off the remainder */
            block_t *rest = (block_t *)((uint8_t *)b + HDR + need);
            rest->size = b->size - need - HDR;
            rest->requested = 0;
            rest->next = b->next; rest->prev = b;
            rest->magic = magic_free(rest);
            if (b->next) b->next->prev = rest;
            b->next = rest;
            b->size = need;
        }
        b->magic = magic_used(b);
        b->requested = size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)size;
        used_payload += b->size;
        if (used_payload > peak_payload) peak_payload = used_payload;
        n_alloc++;
        return payload(b);
    }
    n_failed++;
    return NULL;
}

void *heap_calloc(size_t count, size_t size) {
    if (count != 0 && size > (size_t)-1 / count) { if (first) n_failed++; return NULL; }
    size_t total = count * size;
    void *p = heap_alloc(total);
    if (p) memset(p, 0, total);
    return p;
}

int heap_free(void *ptr) {
    if (!first) return HEAP_ERR_NOT_INIT;
    if (!ptr) return HEAP_ERR_NULL;
    uintptr_t p = (uintptr_t)ptr;
    if (p < heap_lo + HDR || p >= heap_hi) return HEAP_ERR_RANGE;   /* never dereferenced */
    if (p & (HEAP_ALIGN - 1)) return HEAP_ERR_BAD_POINTER;

    block_t *b = (block_t *)(p - HDR);
    if (is_free(b)) return HEAP_ERR_DOUBLE_FREE;
    if (!is_used(b)) return HEAP_ERR_BAD_POINTER;
    if (b->prev ? b->prev->next != b : b != first) return HEAP_ERR_BAD_POINTER;   /* not a real list member */

    used_payload -= b->size;
    memset(payload(b), HEAP_POISON, b->size);
    b->magic = magic_free(b);
    b->requested = 0;
    n_free++;

    if (b->next && is_free(b->next)) {                              /* merge with next */
        block_t *n = b->next;
        b->size += HDR + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
    }
    if (b->prev && is_free(b->prev)) {                              /* merge into prev */
        block_t *pr = b->prev;
        pr->size += HDR + b->size;
        pr->next = b->next;
        if (b->next) b->next->prev = pr;
    }
    return HEAP_OK;
}

bool heap_owns(const void *ptr) {
    uintptr_t p = (uintptr_t)ptr;
    return first && p >= heap_lo && p < heap_hi;
}

size_t heap_block_size(const void *ptr) {
    if (!first) return 0;
    uintptr_t p = (uintptr_t)ptr;
    if (p < heap_lo + HDR || p >= heap_hi || (p & (HEAP_ALIGN - 1))) return 0;
    const block_t *b = (const block_t *)(p - HDR);
    return is_used(b) ? b->size : 0;
}

int heap_check(size_t *bad) {
    size_t dummy; if (!bad) bad = &dummy;
    *bad = 0;
    if (!first) return HEAP_ERR_NOT_INIT;
    size_t idx = 0;
    const block_t *prev = NULL;
    for (const block_t *b = first; b; prev = b, b = b->next, idx++) {
        *bad = idx;
        uintptr_t a = (uintptr_t)b;
        if (a < heap_lo || a + HDR > heap_hi || (a & (HEAP_ALIGN - 1))) return HEAP_ERR_CORRUPT;
        if (!is_used(b) && !is_free(b)) return HEAP_ERR_CORRUPT;
        if (b->prev != prev) return HEAP_ERR_CORRUPT;
        if (b->size == 0 || (b->size & (HEAP_ALIGN - 1))) return HEAP_ERR_CORRUPT;
        if (a + HDR + b->size > heap_hi) return HEAP_ERR_CORRUPT;
        if (b->next) { if ((uintptr_t)b->next != a + HDR + b->size) return HEAP_ERR_CORRUPT; }
        else if (a + HDR + b->size != heap_hi) return HEAP_ERR_CORRUPT;       /* last block must reach the end */
        if (prev && is_free(prev) && is_free(b)) return HEAP_ERR_CORRUPT;      /* coalescing invariant */
    }
    *bad = 0;
    return HEAP_OK;
}

void heap_walk(heap_walk_fn fn, void *ctx) {
    if (!first || !fn) return;
    for (block_t *b = first; b; b = b->next) {
        heap_block_info_t i = { (uintptr_t)payload(b), b->size, b->requested, is_used(b) };
        if (!fn(&i, ctx)) return;
    }
}

void heap_stats(heap_stats_t *s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    if (!first) return;
    s->total_bytes = heap_hi - heap_lo;
    for (const block_t *b = first; b; b = b->next) {
        s->block_count++;
        if (is_used(b)) { s->used_blocks++; s->payload_used += b->size; }
        else {
            s->free_blocks++; s->payload_free += b->size;
            if (b->size > s->largest_free) s->largest_free = b->size;
        }
    }
    s->overhead_bytes = s->block_count * HDR;
    s->peak_payload_used = peak_payload;
    s->alloc_count = n_alloc; s->free_count = n_free; s->failed_allocs = n_failed;
}

size_t heap_header_size(void) { return HDR; }
void heap_region(uintptr_t *start, size_t *size) {
    if (start) *start = heap_lo;
    if (size)  *size = heap_hi - heap_lo;
}

const char *heap_strerror(int c) {
    switch (c) {
        case HEAP_OK:             return "ok";
        case HEAP_ERR_NULL:       return "null pointer";
        case HEAP_ERR_RANGE:      return "address is outside the kernel heap";
        case HEAP_ERR_BAD_POINTER: return "not the start of a live allocation";
        case HEAP_ERR_DOUBLE_FREE: return "block was already freed (double free)";
        case HEAP_ERR_CORRUPT:    return "heap metadata is corrupted";
        case HEAP_ERR_NOT_INIT:   return "heap not initialised";
        case HEAP_ERR_TOO_SMALL:  return "region too small for a heap";
        default:                  return "unknown heap error";
    }
}
