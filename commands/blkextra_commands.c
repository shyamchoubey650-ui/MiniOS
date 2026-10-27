/* More block-device commands (Phase 12): mount, umount, defrag, smart.
 *
 * REAL:       the mount table is real data kept by the shell layer, defrag's numbers come from the live heap, smart's counters
 *             come from the RAM-disk's real read/write counts and the uptime.
 * SIMULATED:  mount/umount attach nothing (MiniFS lives in the kernel heap and has no mount points), defrag moves nothing,
 *             and smart's health attributes other than the counters are made-up constants. Every output says so. */
#include "include/builtins.h"
#include "include/cmdutil.h"
#include "include/screen.h"
#include "include/syscall.h"
#include "include/heap.h"
#include "include/ramdisk.h"
#include "include/timer.h"
#include "include/string.h"

#define MOUNT_MAX 4
static struct { bool used; char dir[64]; uint32_t tick; } mounts[MOUNT_MAX];

static bool is_root(void) { cred_info_t c; return sys_getcred(&c) == 0 && c.euid == 0; }
static bool is_device(const char *s) { return !strcmp(s, "ram0") || !strcmp(s, "/dev/ram0"); }

static int cmd_mount(int argc, const char **argv) {
    if (argc == 1) {
        screen_puts("minifs on / type MiniFS (rw)    [the kernel heap; always mounted]\n");
        for (int i = 0; i < MOUNT_MAX; i++) if (mounts[i].used) { screen_puts("ram0 on "); screen_puts(mounts[i].dir); screen_puts(" type ramdisk (table entry since tick "); screen_put_dec(mounts[i].tick); screen_puts(")\n"); }
        return 0;
    }
    if (argc != 3) return cu_usage("mount   |   mount ram0 <directory>      (root; TABLE ONLY: nothing is attached, MiniFS has no mount points)");
    if (!is_root()) return cu_error("mount", "permission denied: only root can mount (try sudo)");
    if (!is_device(argv[1])) return cu_error("mount", "unknown device: the only block device is ram0 (see lsblk)");
    fs_stat_t st; char path[64];
    if (strlen(argv[2]) >= sizeof path) return cu_error("mount", "path too long");
    int rc = sys_stat(argv[2], &st);
    if (rc != 0) return cu_fs_error("mount", argv[2], rc, CU_DIR);
    if (st.type != FS_DIR) return cu_fs_error("mount", argv[2], SYS_ENOTDIR, CU_DIR);
    memcpy(path, argv[2], strlen(argv[2]) + 1);
    if (!strcmp(path, "/")) return cu_error("mount", "/ is already the MiniFS root");
    for (int i = 0; i < MOUNT_MAX; i++) if (mounts[i].used) {
        if (!strcmp(mounts[i].dir, path)) return cu_error("mount", "something is already mounted there");
        return cu_error("mount", "ram0 is already mounted (umount it first)");
    }
    for (int i = 0; i < MOUNT_MAX; i++) if (!mounts[i].used) {
        mounts[i].used = true; mounts[i].tick = timer_ticks(); memcpy(mounts[i].dir, path, strlen(path) + 1);
        screen_puts("ram0 recorded as mounted on "); screen_puts(path); screen_puts("\n[TABLE ONLY] no files appear there: use 'savefs' / 'loadfs' to move a whole file system to or from ram0\n");
        return 0;
    }
    return cu_error("mount", "the mount table is full");
}

static int cmd_umount(int argc, const char **argv) {
    if (argc != 2) return cu_usage("umount <directory | ram0>      (root; table only)");
    if (!is_root()) return cu_error("umount", "permission denied: only root can unmount (try sudo)");
    if (!strcmp(argv[1], "/") || !strcmp(argv[1], "minifs")) return cu_error("umount", "/ is busy: the root file system cannot be unmounted");
    for (int i = 0; i < MOUNT_MAX; i++)
        if (mounts[i].used && (is_device(argv[1]) || !strcmp(mounts[i].dir, argv[1]))) {
            mounts[i].used = false;
            screen_puts("ram0 unmounted from "); screen_puts(mounts[i].dir); screen_putc('\n');
            return 0;
        }
    return cu_error("umount", "that is not mounted");
}

static int cmd_defrag(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("defrag      (analyses the heap; moves nothing)");
    heap_stats_t h; heap_stats(&h);
    screen_puts("[SIMULATED] MiniFS defragmentation\n");
    const char *pass[] = { "pass 1/3: scanning files...", "pass 2/3: measuring free space...", "pass 3/3: planning moves..." };
    for (int i = 0; i < 3; i++) { screen_puts(pass[i]); screen_putc('\n'); cu_sleep_ms(150); }
    screen_puts("MiniFS stores every file in ONE contiguous heap block, so no file is fragmented and there is nothing to defragment.\n");
    uint32_t free_b = (uint32_t)h.payload_free, largest = (uint32_t)h.largest_free;
    screen_puts("Heap free space: "); cu_put_size(free_b); screen_puts(" in "); screen_put_dec((uint32_t)h.free_blocks); screen_puts(" free block(s); largest block "); cu_put_size(largest);
    if (free_b > 0) { uint32_t frag = 100 - (largest >> 4) * 100 / (free_b >> 4 ? free_b >> 4 : 1); screen_puts(" (free-space fragmentation "); screen_put_dec(frag > 100 ? 0 : frag); screen_puts("%)"); }
    screen_puts("\nNo data was moved.\n");
    return 0;
}

static int cmd_smart(int argc, const char **argv) {
    (void)argv; ramdisk_info_t i;
    if (argc != 1) return cu_usage("smart");
    int rc = sys_blk_info(&i);
    if (rc != 0) return cu_error("smart", sys_strerror(rc));
    uint32_t hours = timer_ticks() / TIMER_HZ / 3600;
    screen_puts("[SIMULATED] S.M.A.R.T. report for ram0\n");
    screen_puts("A RAM-disk has no mechanics: the attributes marked * are real counters, the rest are made-up constants.\n\n");
    screen_puts("ID  attribute               value\n");
    screen_puts("  1 Raw read errors          0\n");
    screen_puts("  5 Reallocated sectors      0\n");
    screen_puts("  9 Power-on hours *         "); screen_put_dec(hours); screen_putc('\n');
    screen_puts("194 Temperature (C)          n/a (RAM)\n");
    screen_puts("241 Sectors written *        "); screen_put_dec(i.writes); screen_putc('\n');
    screen_puts("242 Sectors read *           "); screen_put_dec(i.reads); screen_putc('\n');
    screen_puts("\nOverall health: PASSED (nothing here can fail; run 'badblocks' for a real read/write test)\n");
    return 0;
}

static const struct { const char *name; cmd_handler_t fn; } bindings[] = { { "mount", cmd_mount }, { "umount", cmd_umount }, { "defrag", cmd_defrag }, { "smart", cmd_smart } };

int blkextra_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
