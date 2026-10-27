#ifndef MEMORY_H
#define MEMORY_H
/* Memory manager front end: builds the physical frame map from the Multiboot
 * memory map, carves out the kernel heap, and offers kmalloc()/kfree().
 *
 * REAL:      physical frame tracking, kernel heap, readable-range guard.
 * NOT DONE:  paging / virtual memory / memory protection. Every address in MiniOS
 *            is a physical address (flat model); there is no per-process isolation. */
#include "include/types.h"
#include "include/multiboot.h"

#define MEM_OK              0
#define MEM_ERR_NO_INFO   (-1)   /* bootloader gave neither a memory map nor mem_lower/upper */
#define MEM_ERR_NO_RAM    (-2)   /* no usable RAM found                                     */
#define MEM_ERR_NO_HEAP   (-3)   /* not enough free frames for the heap                     */
#define MEM_ERR_HEAP_INIT (-4)   /* heap_init() refused the region                          */

#define MEM_MAX_REGIONS     16
#define HEAP_DEFAULT_BYTES  (4u * 1024u * 1024u)   /* 4 MiB kernel heap                        */
#define HEAP_MIN_BYTES      (64u * 1024u)

typedef struct { uint64_t base, length; uint32_t type; } mem_region_t;

typedef struct {
    uint32_t ram_bytes;        /* usable RAM (frames * 4 KiB)             */
    uint32_t free_bytes;       /* frames not reserved and not allocated   */
    uint32_t allocated_bytes;  /* frames handed out (the heap region)     */
    uint32_t reserved_bytes;   /* kernel image, first MiB, ...            */
    uint32_t kernel_bytes;     /* size of the kernel image                */
    uint32_t heap_base;
    uint32_t heap_size;
} memory_summary_t;

int  memory_init(const multiboot_info_t *mbi, uintptr_t kernel_start, uintptr_t kernel_end);
const char *memory_strerror(int code);
void memory_summary(memory_summary_t *out);

size_t              memory_region_count(void);          /* stored regions                      */
size_t              memory_regions_seen(void);          /* regions the bootloader reported     */
const mem_region_t *memory_region(size_t index);
const char         *memory_map_source(void);            /* "multiboot memory map" or "mem_lower/mem_upper (no map)" */
const char         *memory_region_type_name(uint32_t type);

/* True only if [addr, addr+len) lies entirely inside the first MiB or inside
 * a region the bootloader reported as available RAM. Guards peek/memdump. */
bool memory_range_readable(uint32_t addr, uint32_t len);

/* The kernel's own code and read-only data must never be written through a system call.
 * kernel.c passes [kernel_start, kernel_data_start); everything else in RAM is writable. */
void memory_set_code_range(uintptr_t start, uintptr_t end);
/* True if [addr, addr+len) is inside available RAM above the first MiB and does not touch the protected code range. */
bool memory_range_writable(uint32_t addr, uint32_t len);

/* Interrupt-safe wrappers around the heap. */
void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
int   kfree(void *ptr);          /* HEAP_OK or HEAP_ERR_* */
#endif
