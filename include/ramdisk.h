#ifndef RAMDISK_H
#define RAMDISK_H
/* RAM-disk: a block device made of kernel-heap memory, 512-byte sectors.
 * REAL:  sector read/write with bounds checks, counters, a saved MiniFS image (savefs/loadfs).
 * NOT:   it is memory, not a disk: its contents vanish at reboot or shutdown. No ATA/IDE driver exists. */
#include "include/types.h"

#define RAMDISK_SECTOR   512u
#define RAMDISK_SECTORS  1024u          /* 512 KiB */

typedef struct {
    uint32_t sector_size, sectors, reads, writes;
    bool     image_valid;               /* a MiniFS image is stored from sector 0 */
    uint32_t image_nodes, image_bytes, image_tick;
} ramdisk_info_t;

int  ramdisk_init(void);                                    /* allocates and zero-fills; 0 or SYS_ENOMEM */
bool ramdisk_ready(void);
int  ramdisk_read(uint32_t sector, void *buf);              /* 0, SYS_EBLOCK (bad sector / not ready) */
int  ramdisk_write(uint32_t sector, const void *buf);
int  ramdisk_peek(uint32_t sector, void *buf);              /* like read, but not counted (used to inspect the image header) */
void ramdisk_info(ramdisk_info_t *out);

/* MiniFS image on the RAM-disk (kernel/filesystem/fsimage.c). Needs root. */
int  fsimage_save(uint32_t *nodes, uint32_t *bytes);        /* 0 or SYS_E* (SYS_ENOSPC if it does not fit) */
int  fsimage_load(uint32_t *nodes);                         /* replaces the whole file system; verifies before touching anything */
void fsimage_describe(ramdisk_info_t *out);                 /* fills the image_* fields from the header */
#endif
