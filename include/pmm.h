#ifndef PMM_H
#define PMM_H
/* Physical memory manager: tracks every 4 KiB page frame of RAM.
 * REAL. One byte of state per frame (256 KiB table for the 1 GiB limit).
 * Uses 32-bit physical addresses; RAM above 1 GiB is ignored (documented limit). */
#include "include/types.h"

#define PMM_FRAME_SIZE  4096u
#define PMM_FRAME_SHIFT 12
#define PMM_MAX_FRAMES  262144u                       /* 1 GiB / 4 KiB */
#define PMM_MAX_ADDR    ((uint64_t)PMM_MAX_FRAMES << PMM_FRAME_SHIFT)

typedef enum {
    PMM_NONRAM   = 0,   /* not usable RAM (hole, device, above the limit) */
    PMM_FREE     = 1,   /* usable and unallocated                         */
    PMM_USED     = 2,   /* handed out by pmm_alloc()                      */
    PMM_RESERVED = 3    /* usable RAM we must never hand out (kernel, BIOS area, ...) */
} pmm_state_t;

#define PMM_OK                 0
#define PMM_ERR_ARG          (-1)   /* count 0, NULL pointer, misaligned address */
#define PMM_ERR_NO_MEMORY    (-2)   /* no run of `count` free frames             */
#define PMM_ERR_NOT_ALLOCATED (-3)  /* freeing a frame that is not currently USED */
#define PMM_ERR_RANGE        (-4)   /* address beyond the managed limit           */

typedef struct {
    uint32_t ram_frames;        /* usable RAM frames = free + used + reserved */
    uint32_t free_frames;
    uint32_t used_frames;       /* allocated through pmm_alloc()              */
    uint32_t reserved_frames;
    uint32_t highest_frame;     /* one past the highest RAM frame seen        */
} pmm_stats_t;

void        pmm_init(void);                                   /* forget everything */
void        pmm_add_region(uint64_t base, uint64_t len);      /* mark whole frames inside as FREE RAM */
void        pmm_reserve(uint64_t base, uint64_t len);         /* FREE frames touching the range -> RESERVED */
int         pmm_alloc(uint32_t count, uint32_t *addr_out);    /* first-fit contiguous run */
int         pmm_free(uint32_t addr, uint32_t count);          /* all-or-nothing */
pmm_state_t pmm_frame_state(uint32_t frame_index);
pmm_stats_t pmm_stats(void);
const char *pmm_strerror(int code);
#endif
