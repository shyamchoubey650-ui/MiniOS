/* Physical frame allocator. Order of use at boot:
 *   pmm_init -> pmm_add_region (every usable RAM range) -> pmm_reserve (kernel, low memory).
 * Reserving before adding a region has no effect on frames that were not RAM yet,
 * so memory_init() always adds first. No 64-bit division is used (the kernel does
 * not link libgcc); only shifts. */
#include "include/pmm.h"

static uint8_t  state[PMM_MAX_FRAMES];
static uint32_t n_ram, n_free, n_used, n_reserved, highest;

void pmm_init(void) {
    for (uint32_t i = 0; i < PMM_MAX_FRAMES; i++) state[i] = PMM_NONRAM;
    n_ram = n_free = n_used = n_reserved = highest = 0;
}

/* Converts [base, base+len) into a frame range. `inner` = only frames fully inside;
 * otherwise every frame the range touches. Returns false if the range is empty. */
static bool frame_range(uint64_t base, uint64_t len, bool inner, uint32_t *first, uint32_t *last) {
    if (len == 0 || base >= PMM_MAX_ADDR) return false;
    uint64_t end = (len > UINT64_MAX - base) ? UINT64_MAX : base + len;   /* saturate on wrap */
    if (end > PMM_MAX_ADDR) end = PMM_MAX_ADDR;
    uint64_t f, l;
    if (inner) { f = (base + PMM_FRAME_SIZE - 1) >> PMM_FRAME_SHIFT; l = end >> PMM_FRAME_SHIFT; }
    else       { f = base >> PMM_FRAME_SHIFT;                         l = (end + PMM_FRAME_SIZE - 1) >> PMM_FRAME_SHIFT; }
    if (l > PMM_MAX_FRAMES) l = PMM_MAX_FRAMES;
    if (f >= l) return false;
    *first = (uint32_t)f; *last = (uint32_t)l;           /* [first, last) */
    return true;
}

void pmm_add_region(uint64_t base, uint64_t len) {
    uint32_t f, l;
    if (!frame_range(base, len, true, &f, &l)) return;
    for (uint32_t i = f; i < l; i++) {
        if (state[i] != PMM_NONRAM) continue;            /* idempotent */
        state[i] = PMM_FREE; n_ram++; n_free++;
    }
    if (l > highest) highest = l;
}

void pmm_reserve(uint64_t base, uint64_t len) {
    uint32_t f, l;
    if (!frame_range(base, len, false, &f, &l)) return;
    for (uint32_t i = f; i < l; i++) {
        if (state[i] != PMM_FREE) continue;
        state[i] = PMM_RESERVED; n_free--; n_reserved++;
    }
}

int pmm_alloc(uint32_t count, uint32_t *addr_out) {
    if (count == 0 || !addr_out) return PMM_ERR_ARG;
    if (count > n_free) return PMM_ERR_NO_MEMORY;
    uint32_t run = 0;
    for (uint32_t i = 0; i < highest; i++) {
        run = (state[i] == PMM_FREE) ? run + 1 : 0;
        if (run == count) {
            uint32_t start = i + 1 - count;
            for (uint32_t k = start; k <= i; k++) state[k] = PMM_USED;
            n_free -= count; n_used += count;
            *addr_out = start << PMM_FRAME_SHIFT;
            return PMM_OK;
        }
    }
    return PMM_ERR_NO_MEMORY;                            /* enough free frames, but not contiguous */
}

int pmm_free(uint32_t addr, uint32_t count) {
    if (count == 0 || (addr & (PMM_FRAME_SIZE - 1))) return PMM_ERR_ARG;
    uint32_t first = addr >> PMM_FRAME_SHIFT;
    if (first >= PMM_MAX_FRAMES || count > PMM_MAX_FRAMES - first) return PMM_ERR_RANGE;
    for (uint32_t i = 0; i < count; i++)                 /* validate everything first */
        if (state[first + i] != PMM_USED) return PMM_ERR_NOT_ALLOCATED;
    for (uint32_t i = 0; i < count; i++) state[first + i] = PMM_FREE;
    n_used -= count; n_free += count;
    return PMM_OK;
}

pmm_state_t pmm_frame_state(uint32_t idx) { return idx < PMM_MAX_FRAMES ? (pmm_state_t)state[idx] : PMM_NONRAM; }

pmm_stats_t pmm_stats(void) {
    pmm_stats_t s = { n_ram, n_free, n_used, n_reserved, highest };
    return s;
}

const char *pmm_strerror(int c) {
    switch (c) {
        case PMM_OK:               return "ok";
        case PMM_ERR_ARG:          return "invalid argument";
        case PMM_ERR_NO_MEMORY:    return "no contiguous run of free frames";
        case PMM_ERR_NOT_ALLOCATED: return "frame is not allocated";
        case PMM_ERR_RANGE:        return "address beyond managed memory";
        default:                   return "unknown PMM error";
    }
}
