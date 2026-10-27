#include "include/memory.h"
#include "include/pmm.h"
#include "include/heap.h"

#ifdef MEMORY_HOST_TEST
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
/* On the host the "physical" heap frames are fake, so back the heap with a static
 * buffer (a non-PIE static address fits in 32 bits, which peek/poke tests need). */
static uint8_t host_heap[HEAP_DEFAULT_BYTES] __attribute__((aligned(4096)));
static void *heap_backing(uint32_t phys, size_t size) {
    (void)phys;
    if (size > sizeof host_heap) return NULL;
    for (size_t i = 0; i < size; i++) host_heap[i] = 0;
    return host_heap;
}
#else
/* Disable interrupts while the heap is modified; restore the previous state after. */
static uint32_t irq_save(void) {
    uint32_t flags;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
static void irq_restore(uint32_t flags) { if (flags & 0x200) __asm__ volatile ("sti" : : : "memory"); }
static void *heap_backing(uint32_t phys, size_t size) { (void)size; return (void *)(uintptr_t)phys; }   /* flat, identity-addressed */
#endif

#define LOW_MEMORY_END 0x100000u          /* first MiB: BIOS, VGA, ROMs - never given out */

static mem_region_t regions[MEM_MAX_REGIONS];
static size_t       n_regions, n_seen;
static const char  *map_source = "none";
static uint32_t     kernel_size, heap_base, heap_size;

static void remember(uint64_t base, uint64_t len, uint32_t type) {
    n_seen++;
    if (n_regions < MEM_MAX_REGIONS) {
        regions[n_regions].base = base; regions[n_regions].length = len; regions[n_regions].type = type;
        n_regions++;
    }
    if (type == MULTIBOOT_MMAP_AVAILABLE) pmm_add_region(base, len);
}

static int scan_memory_map(const multiboot_info_t *mbi) {
    if (mbi->flags & MULTIBOOT_FLAG_MMAP) {
        map_source = "multiboot memory map";
        const uint8_t *p = (const uint8_t *)(uintptr_t)mbi->mmap_addr;
        uint32_t off = 0;
        while (off + 4 <= mbi->mmap_length) {
            const multiboot_mmap_entry_t *e = (const multiboot_mmap_entry_t *)(p + off);
            if (e->size < 20) break;                                  /* malformed entry: stop, keep what we have */
            remember(e->addr, e->len, e->type);
            off += e->size + 4;
        }
    } else if (mbi->flags & MULTIBOOT_FLAG_MEM) {
        map_source = "mem_lower/mem_upper (no map)";
        remember(0, (uint64_t)mbi->mem_lower << 10, MULTIBOOT_MMAP_AVAILABLE);
        remember(LOW_MEMORY_END, (uint64_t)mbi->mem_upper << 10, MULTIBOOT_MMAP_AVAILABLE);
    } else {
        return MEM_ERR_NO_INFO;
    }
    return pmm_stats().ram_frames ? MEM_OK : MEM_ERR_NO_RAM;
}

int memory_init(const multiboot_info_t *mbi, uintptr_t kstart, uintptr_t kend) {
    n_regions = n_seen = 0; map_source = "none"; kernel_size = heap_base = heap_size = 0;
    pmm_init();
    if (!mbi) return MEM_ERR_NO_INFO;

    int rc = scan_memory_map(mbi);
    if (rc != MEM_OK) return rc;

    pmm_reserve(0, LOW_MEMORY_END);                                   /* BIOS/VGA area */
    pmm_reserve(kstart, kend - kstart);                               /* kernel image incl. .bss and boot stack */
    pmm_reserve((uint64_t)(uintptr_t)mbi, sizeof(*mbi));              /* boot info */
    if (mbi->flags & MULTIBOOT_FLAG_MMAP) pmm_reserve(mbi->mmap_addr, mbi->mmap_length);
    kernel_size = (uint32_t)(kend - kstart);

    /* Heap = up to 4 MiB, but never more than half of the free frames. */
    uint32_t free_frames = pmm_stats().free_frames;
    uint32_t heap_frames = HEAP_DEFAULT_BYTES / PMM_FRAME_SIZE;
    if (heap_frames > free_frames / 2) heap_frames = free_frames / 2;
    if (heap_frames * PMM_FRAME_SIZE < HEAP_MIN_BYTES) return MEM_ERR_NO_HEAP;

    uint32_t addr;
    if (pmm_alloc(heap_frames, &addr) != PMM_OK) return MEM_ERR_NO_HEAP;
    void *backing = heap_backing(addr, (size_t)heap_frames * PMM_FRAME_SIZE);
    if (!backing || heap_init(backing, (size_t)heap_frames * PMM_FRAME_SIZE) != HEAP_OK) {
        pmm_free(addr, heap_frames);
        return MEM_ERR_HEAP_INIT;
    }
    heap_base = addr; heap_size = heap_frames * PMM_FRAME_SIZE;
    return MEM_OK;
}

const char *memory_strerror(int c) {
    switch (c) {
        case MEM_OK:            return "ok";
        case MEM_ERR_NO_INFO:   return "bootloader provided no memory information";
        case MEM_ERR_NO_RAM:    return "no usable RAM reported";
        case MEM_ERR_NO_HEAP:   return "not enough free memory for the kernel heap";
        case MEM_ERR_HEAP_INIT: return "heap initialisation failed";
        default:                return "unknown memory error";
    }
}

void memory_summary(memory_summary_t *o) {
    if (!o) return;
    pmm_stats_t s = pmm_stats();
    o->ram_bytes       = s.ram_frames * PMM_FRAME_SIZE;
    o->free_bytes      = s.free_frames * PMM_FRAME_SIZE;
    o->allocated_bytes = s.used_frames * PMM_FRAME_SIZE;
    o->reserved_bytes  = s.reserved_frames * PMM_FRAME_SIZE;
    o->kernel_bytes    = kernel_size;
    o->heap_base       = heap_base;
    o->heap_size       = heap_size;
}

size_t memory_region_count(void) { return n_regions; }
size_t memory_regions_seen(void) { return n_seen; }
const mem_region_t *memory_region(size_t i) { return i < n_regions ? &regions[i] : NULL; }
const char *memory_map_source(void) { return map_source; }

const char *memory_region_type_name(uint32_t t) {
    switch (t) {
        case 1: return "available";
        case 2: return "reserved";
        case 3: return "ACPI reclaimable";
        case 4: return "ACPI NVS";
        case 5: return "bad RAM";
        default: return "unknown";
    }
}

bool memory_range_readable(uint32_t addr, uint32_t len) {
    if (len == 0) return false;
    uint64_t end = (uint64_t)addr + len;                    /* exclusive; cannot overflow in 64 bits */
    if (end <= LOW_MEMORY_END) return true;
    for (size_t i = 0; i < n_regions; i++) {
        const mem_region_t *r = &regions[i];
        if (r->type == MULTIBOOT_MMAP_AVAILABLE && addr >= r->base && end <= r->base + r->length) return true;
    }
    return false;
}

static uint64_t code_lo, code_hi;      /* protected [code_lo, code_hi) */
void memory_set_code_range(uintptr_t start, uintptr_t end) { code_lo = start; code_hi = end; }

bool memory_range_writable(uint32_t addr, uint32_t len) {
    if (len == 0) return false;
    uint64_t end = (uint64_t)addr + len;
    if (addr < LOW_MEMORY_END) return false;                        /* BIOS area, VGA, ROMs */
    if (code_hi > code_lo && addr < code_hi && end > code_lo) return false;
    for (size_t i = 0; i < n_regions; i++) {
        const mem_region_t *r = &regions[i];
        if (r->type == MULTIBOOT_MMAP_AVAILABLE && addr >= r->base && end <= r->base + r->length) return true;
    }
    return false;
}

void *kmalloc(size_t size) {
    uint32_t f = irq_save(); void *p = heap_alloc(size); irq_restore(f); return p;
}
void *kcalloc(size_t count, size_t size) {
    uint32_t f = irq_save(); void *p = heap_calloc(count, size); irq_restore(f); return p;
}
int kfree(void *ptr) {
    uint32_t f = irq_save(); int rc = heap_free(ptr); irq_restore(f); return rc;
}
