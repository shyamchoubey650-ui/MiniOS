/* File-system information and attribute commands (Phase 8): df du mkfs fsck inodes freeblocks
 * fsstat fsdump chmod readonly writable. MiniFS is RAM-backed: there are no disk blocks. */
#include "include/builtins.h"
#include "include/logger.h"
#include "include/cmdutil.h"
#include "include/users.h"
#include "include/screen.h"
#include "include/filesystem.h"
#include "include/heap.h"
#include "include/syscall.h"
#include "include/string.h"

static size_t digits(uint32_t v) { size_t n = 1; while (v >= 10) { v /= 10; n++; } return n; }

static int cmd_df(int argc, const char **argv) {
    (void)argc; (void)argv;
    fs_usage_t u; fs_usage(&u); heap_stats_t h; heap_stats(&h);
    char t[16];
    screen_puts("Filesystem  Type   Nodes        Data        Overhead    Heap free   Mounted on\n");
    cu_pad("minifs", 12); cu_pad("ramfs", 7);
    screen_put_dec(u.nodes); screen_putc('/'); screen_put_dec(u.max_nodes);
    for (size_t k = digits(u.nodes) + 1 + digits(u.max_nodes); k < 13; k++) screen_putc(' ');
    cu_size_str(t, (uint32_t)u.data_bytes); cu_pad(t, 12);
    cu_size_str(t, (uint32_t)(u.capacity_bytes - u.data_bytes + u.node_bytes)); cu_pad(t, 12);
    cu_size_str(t, (uint32_t)h.payload_free); cu_pad(t, 12);
    screen_puts("/\n(volatile: contents are lost when MiniOS stops)\n");
    return 0;
}

static int cmd_du(int argc, const char **argv) {
    if (argc > 2) return cu_usage("du [path]");
    const char *p = argc == 2 ? argv[1] : ".";
    size_t bytes; uint32_t files, dirs;
    int rc = fs_tree_size(p, &bytes, &files, &dirs);
    if (rc != FS_OK) return cu_fs_error("du", p, rc, CU_ANY);
    cu_put_size((uint32_t)bytes); screen_puts("  "); screen_puts(p); screen_puts("  ("); screen_put_dec(files);
    screen_puts(files == 1 ? " file, " : " files, "); screen_put_dec(dirs); screen_puts(dirs == 1 ? " directory)\n" : " directories)\n");
    return 0;
}

static int cmd_mkfs(int argc, const char **argv) {
    if (argc != 2 || strcmp(argv[1], "--yes")) {
        screen_puts("mkfs ERASES every file and directory and recreates the standard layout.\nRun 'mkfs --yes' to confirm.\n");
        return 1;
    }
    uint16_t uid, gid; fs_get_user(&uid, &gid);
    if (uid != FS_UID_ROOT) return cu_error("mkfs", "permission denied: only root may erase the file system (try sudo)");
    int rc = fs_init();
    if (rc != FS_OK) return cu_error("mkfs", fs_strerror(rc));
    users_fs_sync();                                             /* home directories and /etc/passwd are part of the layout */
    klogf(LOG_WARN, "fs", "mkfs: file system erased and re-created (uid %d)", (int)uid);
    screen_puts("MiniFS re-created: standard directories restored, working directory is /\n");
    return 0;
}

static int cmd_fsck(int argc, const char **argv) {
    (void)argc; (void)argv;
    fs_check_report_t r; int problems = fs_check(&r);
    screen_puts("Checked "); screen_put_dec(r.nodes_seen); screen_puts(" nodes: ");
    if (problems == 0) { cu_color(VGA_LIGHT_GREEN); screen_puts("consistent\n"); cu_color(VGA_LIGHT_GREY); return 0; }
    cu_color(VGA_LIGHT_RED); screen_put_dec((uint32_t)problems); screen_puts(" problem(s)\n"); cu_color(VGA_LIGHT_GREY);
    for (int i = 0; i < 4 && i < problems; i++) { screen_puts("  - "); screen_puts(r.first[i]); screen_putc('\n'); }
    return 1;
}

static int cmd_inodes(int argc, const char **argv) {
    (void)argc; (void)argv;
    fs_usage_t u; fs_usage(&u);
    screen_puts("Inodes (nodes): "); screen_put_dec(u.nodes); screen_puts(" used, "); screen_put_dec(u.max_nodes - u.nodes);
    screen_puts(" free, "); screen_put_dec(u.max_nodes); screen_puts(" total\n");
    return 0;
}

static int cmd_freeblocks(int argc, const char **argv) {
    (void)argc; (void)argv;
    heap_stats_t h; heap_stats(&h);
    screen_puts("MiniFS has no disk blocks: file data lives in kernel-heap buffers.\nBytes still available for file data (heap free): ");
    screen_put_dec((uint32_t)h.payload_free); screen_puts("  largest single piece: "); screen_put_dec((uint32_t)h.largest_free); screen_putc('\n');
    return 0;
}

static int cmd_fsstat(int argc, const char **argv) {
    (void)argc; (void)argv;
    fs_usage_t u; fs_usage(&u);
    screen_puts("MiniFS statistics\n");
    screen_puts("  Files        : "); screen_put_dec(u.files); screen_putc('\n');
    screen_puts("  Directories  : "); screen_put_dec(u.dirs); screen_putc('\n');
    screen_puts("  Data stored  : "); cu_put_size((uint32_t)u.data_bytes); screen_puts(" in "); cu_put_size((uint32_t)u.capacity_bytes); screen_puts(" of buffers\n");
    screen_puts("  Node records : "); cu_put_size((uint32_t)u.node_bytes); screen_putc('\n');
    screen_puts("  Limits       : "); screen_put_dec(FS_MAX_NODES); screen_puts(" nodes, file size "); cu_put_size(FS_MAX_FILE_SIZE);
    screen_puts(", name "); screen_put_dec(FS_NAME_MAX); screen_puts(" chars, depth "); screen_put_dec(FS_MAX_DEPTH); screen_putc('\n');
    return 0;
}

static bool dump_cb(const fs_dirent_t *e, void *c) {
    (void)c; char m[11]; fs_mode_string(e->type, e->mode, m);
    screen_puts("    inode "); cu_num_pad(e->ino, 3); screen_puts("  "); screen_puts(m); screen_puts("  "); screen_puts(e->name);
    if (e->type == FS_DIR) screen_putc('/');
    screen_putc('\n');
    return true;
}
static int cmd_fsdump(int argc, const char **argv) {
    (void)argc; (void)argv;
    fs_stat_t root;
    fs_stat("/", &root);
    screen_puts("MiniFS layout\n  Storage    : RAM only (volatile), tree of heap-allocated nodes\n");
    screen_puts("  Root inode : "); screen_put_dec(root.ino); screen_puts(", "); screen_put_dec(root.children); screen_puts(" entries\n");
    screen_puts("  Persistence: none\n  Root directory:\n");
    fs_readdir("/", dump_cb, NULL);
    return 0;
}

/* ---------- chmod and friends ---------- */
static bool parse_octal(const char *s, uint16_t *mode) {
    uint32_t v = 0; size_t n = 0;
    for (; s[n]; n++) { if (s[n] < '0' || s[n] > '7') return false; v = v * 8 + (uint32_t)(s[n] - '0'); if (v > 0777) return false; }
    if (n == 0 || n > 4) return false;
    *mode = (uint16_t)v;
    return true;
}

/* Symbolic modes like +x, -w, u+x, go-w, a=r. */
static bool apply_symbolic(const char *spec, uint16_t *mode) {
    const char *p = spec; uint16_t who = 0;
    for (; *p == 'u' || *p == 'g' || *p == 'o' || *p == 'a'; p++) who |= (*p == 'u') ? 0700 : (*p == 'g') ? 070 : (*p == 'o') ? 07 : 0777;
    if (!who) who = 0777;
    if (*p != '+' && *p != '-' && *p != '=') return false;
    char op = *p++; uint16_t bits = 0;
    if (!*p) return false;
    for (; *p; p++) { if (*p == 'r') bits |= 0444; else if (*p == 'w') bits |= 0222; else if (*p == 'x') bits |= 0111; else return false; }
    bits &= who;
    if (op == '+') *mode |= bits; else if (op == '-') *mode &= (uint16_t)~bits; else *mode = (uint16_t)((*mode & ~who) | bits);
    return true;
}

static int cmd_chmod(int argc, const char **argv) {
    if (argc < 3) return cu_usage("chmod <mode> <path...>   (octal like 644, or +x -w u+r go-w)");
    uint16_t fixed = 0; bool octal = parse_octal(argv[1], &fixed);
    int status = 0;
    if (!octal) { uint16_t probe = 0; if (!apply_symbolic(argv[1], &probe)) return cu_error("chmod", "bad mode (use octal such as 755, or symbolic such as u+x)"); }
    for (int i = 2; i < argc; i++) {
        fs_stat_t st; int rc = sys_stat(argv[i], &st);
        if (rc == 0) { uint16_t m = octal ? fixed : st.mode; if (!octal) apply_symbolic(argv[1], &m); rc = sys_chmod(argv[i], m); }
        if (rc != 0) status = cu_fs_error("chmod", argv[i], rc, CU_ANY);
    }
    return status;
}

static int change_write(const char *cmd, int argc, const char **argv, bool allow) {
    if (argc < 2) { char u[40]; size_t n = strlen(cmd); memcpy(u, cmd, n); memcpy(u + n, " <path...>", 11); return cu_usage(u); }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        fs_stat_t st; int rc = sys_stat(argv[i], &st);
        if (rc == 0) rc = sys_chmod(argv[i], allow ? (uint16_t)(st.mode | 0200) : (uint16_t)(st.mode & ~0222));
        if (rc != 0) status = cu_fs_error(cmd, argv[i], rc, CU_ANY);
    }
    return status;
}
static int cmd_readonly(int argc, const char **argv) { return change_write("readonly", argc, argv, false); }
static int cmd_writable(int argc, const char **argv) { return change_write("writable", argc, argv, true); }

static const struct { const char *name; cmd_handler_t fn; } bindings[] = {
    { "df", cmd_df }, { "du", cmd_du }, { "mkfs", cmd_mkfs }, { "fsck", cmd_fsck }, { "inodes", cmd_inodes },
    { "freeblocks", cmd_freeblocks }, { "fsstat", cmd_fsstat }, { "fsdump", cmd_fsdump }, { "chmod", cmd_chmod },
    { "readonly", cmd_readonly }, { "writable", cmd_writable },
};

int fsinfo_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
