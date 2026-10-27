/* Disk and storage commands (Phase 11): lsblk diskinfo ramdisk blkread blkwrite blkdump sync savefs loadfs fdisk diskbench badblocks.
 * The only block device is the RAM-disk (include/ramdisk.h): REAL sector I/O on heap memory. There is NO ATA/IDE driver.
 * Raw sector access needs root: it bypasses file permissions. savefs/loadfs copy MiniFS to/from the RAM-disk (it does not survive reboot). */
#include "include/builtins.h"
#include "include/cmdutil.h"
#include "include/screen.h"
#include "include/syscall.h"
#include "include/ramdisk.h"
#include "include/numparse.h"
#include "include/string.h"

static int blk_error(const char *cmd, int rc) {
    if (rc == SYS_EACCES) return cu_error(cmd, "raw device access needs root (use 'sudo').");
    return cu_error(cmd, sys_strerror(rc));
}
static void label(const char *s) { cu_pad(s, 16); screen_puts(": "); }

static int cmd_lsblk(int argc, const char **argv) {
    (void)argv; ramdisk_info_t i;
    if (argc != 1) return cu_usage("lsblk");
    int rc = sys_blk_info(&i);
    if (rc != 0) return blk_error("lsblk", rc);
    screen_puts("NAME  TYPE  SECTORS  SIZE      CONTENT\n");
    cu_pad("ram0", 5); screen_putc(' '); cu_pad("ram", 5); screen_putc(' '); cu_num_pad(i.sectors, 7); screen_puts("  "); cu_put_size(i.sectors * i.sector_size); screen_puts("   ");
    screen_puts(i.image_valid ? "MiniFS image (see diskinfo)" : "empty (no saved image)"); screen_putc('\n');
    screen_puts("(MiniFS itself lives in the kernel heap, not on ram0; no ATA/IDE disk driver exists)\n");
    return 0;
}

static int cmd_diskinfo(int argc, const char **argv) {
    (void)argv; ramdisk_info_t i;
    if (argc != 1) return cu_usage("diskinfo");
    int rc = sys_blk_info(&i);
    if (rc != 0) return blk_error("diskinfo", rc);
    label("Device"); screen_puts("ram0 (RAM-disk, volatile)\n");
    label("Sector size"); screen_put_dec(i.sector_size); screen_puts(" bytes\n");
    label("Sectors"); screen_put_dec(i.sectors); screen_puts("  ("); cu_put_size(i.sectors * i.sector_size); screen_puts(")\n");
    label("Sector reads"); screen_put_dec(i.reads); screen_putc('\n');
    label("Sector writes"); screen_put_dec(i.writes); screen_putc('\n');
    label("Saved image"); if (i.image_valid) { screen_put_dec(i.image_nodes); screen_puts(" nodes, "); cu_put_size(i.image_bytes); screen_puts(", written at tick "); screen_put_dec(i.image_tick); } else screen_puts("none"); screen_putc('\n');
    return 0;
}

static int cmd_ramdisk(int argc, const char **argv) {
    ramdisk_info_t i;
    if (argc == 2 && !strcmp(argv[1], "clear")) {
        uint8_t zero[RAMDISK_SECTOR]; memset(zero, 0, sizeof zero);
        int rc = sys_blk_info(&i);
        if (rc != 0) return blk_error("ramdisk", rc);
        for (uint32_t s = 0; s < i.sectors; s++) { rc = sys_blk_write(s, zero); if (rc != 0) return blk_error("ramdisk", rc); }
        screen_puts("RAM-disk erased ("); screen_put_dec(i.sectors); screen_puts(" sectors set to zero; any saved image is gone).\n");
        return 0;
    }
    if (argc != 1) return cu_usage("ramdisk [clear]");
    int rc = sys_blk_info(&i);
    if (rc != 0) return blk_error("ramdisk", rc);
    cu_put_size(i.sectors * i.sector_size); screen_puts(" RAM-disk, "); screen_puts(i.image_valid ? "holds a MiniFS image" : "empty"); screen_puts("; run 'ramdisk clear' (root) to erase it\n");
    return 0;
}

static bool parse_sector(const char *cmd, const char *s, uint32_t *n) {
    if (!cu_parse_count(s, n, RAMDISK_SECTORS - 1)) { cu_error(cmd, "sector must be a number from 0 to 1023."); return false; }
    return true;
}

static int cmd_blkread(int argc, const char **argv) {
    uint32_t n; uint8_t b[RAMDISK_SECTOR];
    if (argc != 2) return cu_usage("blkread <sector>     (shows the sector as text; non-printable bytes as '.')");
    if (!parse_sector("blkread", argv[1], &n)) return 1;
    int rc = sys_blk_read(n, b);
    if (rc != 0) return blk_error("blkread", rc);
    for (size_t i = 0; i < sizeof b; i++) { screen_putc(b[i] >= 32 && b[i] < 127 ? (char)b[i] : '.'); if (i % 64 == 63) screen_putc('\n'); }
    return 0;
}

static int cmd_blkdump(int argc, const char **argv) {
    uint32_t n, bytes = 128; uint8_t b[RAMDISK_SECTOR];
    if (argc < 2 || argc > 3) return cu_usage("blkdump <sector> [bytes 16-512]     (hex + ASCII)");
    if (!parse_sector("blkdump", argv[1], &n)) return 1;
    if (argc == 3 && (!cu_parse_count(argv[2], &bytes, 512) || bytes < 16)) return cu_error("blkdump", "bytes must be 16 to 512.");
    int rc = sys_blk_read(n, b);
    if (rc != 0) return blk_error("blkdump", rc);
    static const char hx[] = "0123456789abcdef";
    for (uint32_t off = 0; off < bytes; off += 16) {
        screen_put_hex(n * RAMDISK_SECTOR + off); screen_puts(": ");
        for (uint32_t k = 0; k < 16; k++) { if (off + k < bytes) { screen_putc(hx[b[off + k] >> 4]); screen_putc(hx[b[off + k] & 15]); } else screen_puts("  "); screen_putc(' '); }
        screen_putc(' ');
        for (uint32_t k = 0; k < 16 && off + k < bytes; k++) screen_putc(b[off + k] >= 32 && b[off + k] < 127 ? (char)b[off + k] : '.');
        screen_putc('\n');
    }
    return 0;
}

static int cmd_blkwrite(int argc, const char **argv) {
    uint32_t n; uint8_t b[RAMDISK_SECTOR];
    if (argc < 3) return cu_usage("blkwrite <sector> <text...>     (root; the rest of the sector is zeroed)");
    if (!parse_sector("blkwrite", argv[1], &n)) return 1;
    memset(b, 0, sizeof b); size_t o = 0;
    for (int i = 2; i < argc; i++) {
        size_t l = strlen(argv[i]);
        if (o + l + 1 > sizeof b) return cu_error("blkwrite", "text longer than one 512-byte sector.");
        if (i > 2) b[o++] = ' ';
        memcpy(b + o, argv[i], l); o += l;
    }
    int rc = sys_blk_write(n, b);
    if (rc != 0) return blk_error("blkwrite", rc);
    screen_puts("Wrote "); screen_put_dec((uint32_t)o); screen_puts(" bytes to sector "); screen_put_dec(n); screen_puts(" (a saved MiniFS image in sector 0-N would now fail its checksum)\n");
    return 0;
}

static int cmd_sync(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("sync");
    screen_puts("Nothing to flush: MiniFS keeps no write cache and RAM-disk writes are immediate. Use 'savefs' to copy MiniFS to the RAM-disk.\n");
    return 0;
}

static int cmd_savefs(int argc, const char **argv) {
    (void)argv; uint32_t bytes = 0;
    if (argc != 1) return cu_usage("savefs     (root: copies all files, owners, modes and protection flags to the RAM-disk)");
    int n = sys_fs_save(&bytes);
    if (n == SYS_ENOSPC) return cu_error("savefs", "the file system does not fit on the RAM-disk (512 KiB).");
    if (n < 0) return blk_error("savefs", n);
    screen_puts("Saved "); screen_put_dec((uint32_t)n); screen_puts(" files/directories ("); cu_put_size(bytes); screen_puts(") to ram0. It lasts until reboot or shutdown.\n");
    return 0;
}

static int cmd_loadfs(int argc, const char **argv) {
    if (argc != 2 || strcmp(argv[1], "--yes")) {
        if (argc == 1) { cu_color(VGA_YELLOW); screen_puts("loadfs REPLACES the whole file system with the image on ram0.\nType 'loadfs --yes' to continue.\n"); cu_color(VGA_LIGHT_GREY); return 1; }
        return cu_usage("loadfs --yes     (root: restores the image written by savefs)");
    }
    int n = sys_fs_load();
    if (n == SYS_ENOENT) return cu_error("loadfs", "there is no saved image on ram0 (run 'savefs' first).");
    if (n == SYS_EINVAL) return cu_error("loadfs", "the image is damaged (checksum or format check failed); the file system was NOT changed.");
    if (n < 0) return blk_error("loadfs", n);
    screen_puts("Restored "); screen_put_dec((uint32_t)n); screen_puts(" files/directories from ram0. The working directory is now /.\n");
    return 0;
}

static int cmd_fdisk(int argc, const char **argv) {
    (void)argv; ramdisk_info_t i;
    if (argc != 1) return cu_usage("fdisk");
    int rc = sys_blk_info(&i);
    if (rc != 0) return blk_error("fdisk", rc);
    screen_puts("Disk ram0: "); cu_put_size(i.sectors * i.sector_size); screen_puts(", "); screen_put_dec(i.sectors); screen_puts(" sectors of "); screen_put_dec(i.sector_size); screen_puts(" bytes\n");
    screen_puts("Partition table: none (no MBR/GPT; the whole device is used as one unit, sectors 0-"); screen_put_dec(i.sectors - 1); screen_puts(")\n");
    screen_puts("Sector 0 holds the MiniFS image header when 'savefs' has been run.\n");
    return 0;
}

static uint32_t first_free_sector(const ramdisk_info_t *i) { return i->image_valid ? (i->image_bytes + RAMDISK_SECTOR - 1) / RAMDISK_SECTOR : 0; }

static int cmd_diskbench(int argc, const char **argv) {
    (void)argv; ramdisk_info_t i; uint8_t b[RAMDISK_SECTOR];
    if (argc != 1) return cu_usage("diskbench     (root; reads every sector 10 times, writes only sectors beyond a saved image)");
    int rc = sys_blk_info(&i);
    if (rc != 0) return blk_error("diskbench", rc);
    uint32_t t0 = sys_time();
    for (int pass = 0; pass < 10; pass++) for (uint32_t s = 0; s < i.sectors; s++) { rc = sys_blk_read(s, b); if (rc != 0) return blk_error("diskbench", rc); }
    uint32_t t1 = sys_time();
    screen_puts("Read : "); screen_put_dec(10 * i.sectors); screen_puts(" sectors ("); cu_put_size(10 * i.sectors * i.sector_size); screen_puts(") in "); screen_put_dec((t1 - t0) * 10); screen_puts(" ms (timer resolution 10 ms)\n");
    uint32_t from = first_free_sector(&i), n = i.sectors > from ? i.sectors - from : 0;
    if (n > 128) { from = i.sectors - 128; n = 128; }
    if (n == 0) { screen_puts("Write: skipped (a saved image fills the RAM-disk)\n"); return 0; }
    memset(b, 0xA5, sizeof b);
    for (uint32_t s = 0; s < n; s++) { rc = sys_blk_write(from + s, b); if (rc != 0) return blk_error("diskbench", rc); }
    uint32_t t2 = sys_time();
    screen_puts("Write: "); screen_put_dec(n); screen_puts(" sectors in "); screen_put_dec((t2 - t1) * 10); screen_puts(" ms (the free sectors "); screen_put_dec(from); screen_putc('-'); screen_put_dec(from + n - 1); screen_puts(" now contain 0xA5)\n");
    screen_puts("This measures memory copies in the kernel heap, not a physical disk.\n");
    return 0;
}

static int cmd_badblocks(int argc, const char **argv) {
    (void)argv; ramdisk_info_t i; uint8_t orig[RAMDISK_SECTOR], pat[RAMDISK_SECTOR], back[RAMDISK_SECTOR];
    if (argc != 1) return cu_usage("badblocks     (root; reads every sector, and write-tests the free ones, restoring their contents)");
    int rc = sys_blk_info(&i);
    if (rc != 0) return blk_error("badblocks", rc);
    uint32_t bad = 0, tested = 0, from = first_free_sector(&i);
    for (uint32_t s = 0; s < i.sectors; s++) {
        rc = sys_blk_read(s, orig);
        if (rc != 0) { if (rc == SYS_EACCES) return blk_error("badblocks", rc); bad++; continue; }
        if (s < from) continue;                                       /* sectors holding the saved image are only read, never written */
        bool ok = true;
        for (int p = 0; p < 2 && ok; p++) {
            memset(pat, p ? 0x55 : 0xAA, sizeof pat);
            ok = sys_blk_write(s, pat) == 0 && sys_blk_read(s, back) == 0 && !memcmp(back, pat, sizeof pat);
        }
        sys_blk_write(s, orig);                                       /* restore */
        tested++;
        if (!ok) bad++;
    }
    screen_put_dec(i.sectors); screen_puts(" sectors read, "); screen_put_dec(tested); screen_puts(" write-tested (0xAA/0x55, then restored), "); screen_put_dec(bad); screen_puts(" bad\n");
    return bad ? 1 : 0;
}

static const struct { const char *name; cmd_handler_t fn; } bindings[] = {
    { "lsblk", cmd_lsblk }, { "diskinfo", cmd_diskinfo }, { "ramdisk", cmd_ramdisk }, { "blkread", cmd_blkread }, { "blkwrite", cmd_blkwrite },
    { "blkdump", cmd_blkdump }, { "sync", cmd_sync }, { "savefs", cmd_savefs }, { "loadfs", cmd_loadfs }, { "fdisk", cmd_fdisk },
    { "diskbench", cmd_diskbench }, { "badblocks", cmd_badblocks },
};

int disk_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
