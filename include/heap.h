#ifndef HEAP_H
#define HEAP_H
/* Kernel heap: first-fit allocator over one contiguous region.
 * REAL. Every block has a 32-byte header; blocks are kept in address order in a
 * doubly linked list, and neighbouring free blocks are merged on free().
 * NOT thread/interrupt safe by itself: memory.c's kmalloc()/kfree() wrap it. */
#include "include/types.h"

#define HEAP_ALIGN 16u

#define HEAP_OK                0
#define HEAP_ERR_NULL        (-1)   /* NULL pointer or NULL base                         */
#define HEAP_ERR_RANGE       (-2)   /* pointer is outside the heap                       */
#define HEAP_ERR_BAD_POINTER (-3)   /* misaligned, or not the start of a live block      */
#define HEAP_ERR_DOUBLE_FREE (-4)   /* block is already free                             */
#define HEAP_ERR_CORRUPT     (-5)   /* heap_check() found damaged headers                */
#define HEAP_ERR_NOT_INIT    (-6)
#define HEAP_ERR_TOO_SMALL   (-7)   /* region cannot hold even one block                 */

typedef struct {
    size_t   total_bytes;        /* size of the managed region                       */
    size_t   payload_used;       /* bytes in allocated blocks (rounded sizes)        */
    size_t   payload_free;       /* bytes in free blocks                             */
    size_t   overhead_bytes;     /* headers: block_count * header size               */
    size_t   block_count, used_blocks, free_blocks;
    size_t   largest_free;       /* biggest single free payload                      */
    size_t   peak_payload_used;
    uint32_t alloc_count;        /* successful heap_alloc() calls                    */
    uint32_t free_count;         /* successful heap_free() calls                     */
    uint32_t failed_allocs;      /* heap_alloc() calls that returned NULL (size > 0) */
} heap_stats_t;
/* invariant: total_bytes == payload_used + payload_free + overhead_bytes */

typedef struct {
    uintptr_t addr;              /* payload address                                  */
    size_t    size;              /* payload size (multiple of HEAP_ALIGN)            */
    size_t    requested;         /* bytes the caller asked for (0 for free blocks)   */
    bool      used;
} heap_block_info_t;

int     heap_init(void *base, size_t size);
void   *heap_alloc(size_t size);                 /* NULL on size 0, exhaustion or not initialised */
void   *heap_calloc(size_t count, size_t size);  /* zeroed; NULL on overflow */
int     heap_free(void *ptr);                    /* HEAP_OK or HEAP_ERR_* */
size_t  heap_block_size(const void *ptr);        /* payload size of a live block, else 0 */
bool    heap_owns(const void *ptr);              /* inside the heap region? */
int     heap_check(size_t *bad_block_index);     /* HEAP_OK or HEAP_ERR_CORRUPT */
void    heap_stats(heap_stats_t *out);
size_t  heap_header_size(void);
void    heap_region(uintptr_t *start, size_t *size);

typedef bool (*heap_walk_fn)(const heap_block_info_t *block, void *ctx);   /* return false to stop */
void    heap_walk(heap_walk_fn fn, void *ctx);

const char *heap_strerror(int code);
#endif
