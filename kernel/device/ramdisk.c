/* RAM-disk block device. See include/ramdisk.h. */
#include "include/ramdisk.h"
#include "include/syscall.h"
#include "include/memory.h"
#include "include/string.h"

static uint8_t *disk;
static uint32_t n_reads, n_writes;

int ramdisk_init(void) {
    disk = kcalloc(RAMDISK_SECTORS, RAMDISK_SECTOR);              /* called once at boot (the old pointer is never reused, even if the heap was reset) */
    if (!disk) return SYS_ENOMEM;
    n_reads = n_writes = 0;
    return 0;
}
bool ramdisk_ready(void) { return disk != NULL; }

int ramdisk_peek(uint32_t sector, void *buf) {
    if (!disk || sector >= RAMDISK_SECTORS || !buf) return SYS_EBLOCK;
    memcpy(buf, disk + (size_t)sector * RAMDISK_SECTOR, RAMDISK_SECTOR);
    return 0;
}
int ramdisk_read(uint32_t sector, void *buf) {
    int rc = ramdisk_peek(sector, buf);
    if (rc == 0) n_reads++;
    return rc;
}
int ramdisk_write(uint32_t sector, const void *buf) {
    if (!disk || sector >= RAMDISK_SECTORS || !buf) return SYS_EBLOCK;
    memcpy(disk + (size_t)sector * RAMDISK_SECTOR, buf, RAMDISK_SECTOR);
    n_writes++;
    return 0;
}
void ramdisk_info(ramdisk_info_t *o) {
    memset(o, 0, sizeof *o);
    o->sector_size = RAMDISK_SECTOR; o->sectors = disk ? RAMDISK_SECTORS : 0; o->reads = n_reads; o->writes = n_writes;
    fsimage_describe(o);
}
