/* File commands (Phase 8; most moved onto system calls in Phase 9): touch write append rm cp mv rename stat file truncate exists filesize
 * mkfile readfile backup restore swapfiles fill touchn trash undelete emptytrash shred note.
 * Everything operates on real MiniFS state. MiniFS is RAM-backed and volatile. */
#include "include/builtins.h"
#include "include/cmdutil.h"
#include "include/screen.h"
#include "include/filesystem.h"
#include "include/textutil.h"
#include "include/timer.h"
#include "include/memory.h"
#include "include/syscall.h"
#include "include/numparse.h"
#include "include/string.h"

#define LINE_BUF 256

static bool strchr_slash(const char *s) { for (; *s; s++) if (*s == '/') return true; return false; }

/* Absolute canonical path for `path`, using the working directory obtained through a system call. */
static int resolve(const char *path, char *out, size_t cap) {
    char cwd[FS_PATH_MAX];
    int rc = sys_getcwd(cwd, sizeof cwd);
    if (rc < 0) return rc;
    return fs_path_normalize(path, cwd, out, cap);
}
#define TRASH_DIR "/tmp/.trash"

/* Joins argv[from..] with single spaces and a trailing newline. Returns length. */
static size_t join_args(int argc, const char **argv, int from, char *out, size_t cap) {
    size_t n = 0;
    for (int i = from; i < argc && n + 2 < cap; i++) {
        if (i > from) out[n++] = ' ';
        for (const char *s = argv[i]; *s && n + 2 < cap; s++) out[n++] = *s;
    }
    out[n++] = '\n';
    return n;
}

static int cmd_touch(int argc, const char **argv) {
    if (argc < 2) return cu_usage("touch <file...>");
    int status = 0;
    for (int i = 1; i < argc; i++) { int rc = sys_touch(argv[i]); if (rc != 0) status = cu_fs_error("touch", argv[i], rc, CU_ANY); }
    return status;
}

/* Writes `len` bytes to a file using open/write/close. */
static int put_file(const char *cmd, const char *path, const char *data, size_t len, bool append) {
    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREATE | (append ? SYS_O_APPEND : SYS_O_TRUNC));
    if (fd < 0) return cu_fs_error(cmd, path, fd, CU_FILE);
    int n = sys_write(fd, data, len);
    sys_close(fd);
    if (n < 0) return cu_fs_error(cmd, path, n, CU_FILE);
    return 0;
}

static int write_text(const char *cmd, const char *usage, int argc, const char **argv, fs_write_mode_t mode) {
    if (argc < 3) return cu_usage(usage);
    char line[LINE_BUF]; size_t n = join_args(argc, argv, 2, line, sizeof line);
    return put_file(cmd, argv[1], line, n, mode == FS_WRITE_APPEND);
}
static int cmd_write(int argc, const char **argv)  { return write_text("write", "write <file> <text...>", argc, argv, FS_WRITE_TRUNCATE); }
static int cmd_append(int argc, const char **argv) { return write_text("append", "append <file> <text...>", argc, argv, FS_WRITE_APPEND); }

static int cmd_note(int argc, const char **argv) {
    if (argc < 3) return cu_usage("note <file> <text...>   (appends a time-stamped line)");
    char line[LINE_BUF], head[24]; size_t n = 0;
    uint32_t t = sys_time() / TIMER_HZ;
    head[n++] = '['; head[n++] = '+';
    char d[12]; size_t k = 0; do { d[k++] = (char)('0' + t % 10); t /= 10; } while (t);
    while (k) head[n++] = d[--k];
    head[n++] = 's'; head[n++] = ']'; head[n++] = ' '; head[n] = '\0';
    size_t hl = n; memcpy(line, head, hl);
    size_t m = join_args(argc, argv, 2, line + hl, sizeof line - hl);
    return put_file("note", argv[1], line, hl + m, true);
}

static int cmd_rm(int argc, const char **argv) {
    if (argc < 2) return cu_usage("rm <file...>");
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int rc = sys_delete(argv[i]);
        if (rc == SYS_EISDIR) { cu_error("rm", "is a directory (use rmdir, or rmtree for non-empty ones)"); status = 1; }
        else if (rc != 0) status = cu_fs_error("rm", argv[i], rc, CU_FILE);
    }
    return status;
}

/* If `dst` is an existing directory, the real destination is dst/<basename(src)>. */
static void real_dest(const char *src, const char *dst, char *out, size_t cap) {
    fs_stat_t st; char base[FS_PATH_MAX];
    size_t i = 0; const char *b = fs_path_basename(src);
    while (b[i] && b[i] != '/' && i < FS_NAME_MAX) { base[i] = b[i]; i++; }
    base[i] = '\0';
    if (sys_stat(dst, &st) == 0 && st.type == FS_DIR) {
        size_t n = strlen(dst);
        if (n + 1 + i + 1 > cap) { out[0] = '\0'; return; }
        memcpy(out, dst, n); if (n && dst[n - 1] != '/') out[n++] = '/';
        memcpy(out + n, base, i + 1);
    } else { size_t n = strlen(dst); if (n + 1 > cap) n = cap - 1; memcpy(out, dst, n); out[n] = '\0'; }
}

static int cmd_cp(int argc, const char **argv) {
    bool recursive = argc >= 2 && !strcmp(argv[1], "-r");
    int a = recursive ? 2 : 1;
    if (argc - a != 2) return cu_usage("cp [-r] <source> <destination>");
    char dest[FS_PATH_MAX]; real_dest(argv[a], argv[a + 1], dest, sizeof dest);
    fs_stat_t st; int rc = sys_stat(argv[a], &st);
    if (rc != 0) return cu_fs_error("cp", argv[a], rc, CU_FILE);
    if (st.type == FS_DIR) {
        if (!recursive) return cu_error("cp", "source is a directory (use cp -r)");
        rc = fs_copy_tree(argv[a], dest);
    } else rc = fs_copy(argv[a], dest);
    if (rc == FS_ERR_INTO_SELF) return cu_error("cp", "cannot copy a directory into itself");
    if (rc == FS_ERR_EXISTS && st.type == FS_FILE) return cu_error("cp", "source and destination are the same file");
    return rc == FS_OK ? 0 : cu_fs_error("cp", dest, rc, CU_ANY);
}

static int cmd_mv(int argc, const char **argv) {
    if (argc != 3) return cu_usage("mv <source> <destination>");
    char dest[FS_PATH_MAX]; real_dest(argv[1], argv[2], dest, sizeof dest);
    int rc = sys_rename(argv[1], dest);
    if (rc == SYS_EINTOSELF) return cu_error("mv", "cannot move a directory into itself");
    if (rc == SYS_EBUSY) return cu_error("mv", "cannot move the root directory");
    if (rc == SYS_ENOENT) { fs_stat_t st; if (sys_stat(argv[1], &st) != 0) return cu_fs_error("mv", argv[1], rc, CU_FILE); }
    return rc == 0 ? 0 : cu_fs_error("mv", dest, rc, CU_ANY);
}

static int cmd_rename(int argc, const char **argv) {
    if (argc != 3) return cu_usage("rename <path> <new-name>   (new name only, no directory part)");
    if (strchr_slash(argv[2])) return cu_error("rename", "the new name must not contain '/' (use mv to move)");
    char canon[FS_PATH_MAX], parent[FS_PATH_MAX], name[FS_NAME_MAX + 1], target[FS_PATH_MAX];
    int rc = resolve(argv[1], canon, sizeof canon);
    if (rc == FS_OK) rc = fs_path_split(canon, parent, sizeof parent, name, sizeof name);
    if (rc != FS_OK) return cu_fs_error("rename", argv[1], rc == FS_ERR_INVALID ? FS_ERR_BUSY : rc, CU_ANY);
    size_t pl = strlen(parent), nl = strlen(argv[2]);
    if (pl + 1 + nl + 1 > sizeof target) return cu_fs_error("rename", argv[2], FS_ERR_PATH_TOO_LONG, CU_ANY);
    memcpy(target, parent, pl); size_t n = pl; if (pl > 1) target[n++] = '/';
    memcpy(target + n, argv[2], nl + 1);
    rc = sys_rename(canon, target);
    return rc == 0 ? 0 : cu_fs_error("rename", rc == SYS_ENOENT ? argv[1] : argv[2], rc, CU_ANY);
}

static int cmd_stat(int argc, const char **argv) {
    if (argc != 2) return cu_usage("stat <path>");
    fs_stat_t st; int rc = sys_stat(argv[1], &st);
    if (rc != 0) return cu_fs_error("stat", argv[1], rc, CU_ANY);
    char m[11]; fs_mode_string(st.type, st.mode, m);
    screen_puts("  Name   : "); screen_puts(st.name); screen_putc('\n');
    screen_puts("  Type   : "); screen_puts(st.type == FS_DIR ? "directory" : "regular file"); screen_putc('\n');
    screen_puts("  Inode  : "); screen_put_dec(st.ino); screen_putc('\n');
    screen_puts("  Mode   : 0"); screen_put_dec((st.mode >> 6) & 7); screen_put_dec((st.mode >> 3) & 7); screen_put_dec(st.mode & 7);
    screen_puts("  ("); screen_puts(m); screen_puts(")\n");
    { char un[20], gn[20]; cu_uid_name(st.uid, un, sizeof un); cu_uid_name(st.gid, gn, sizeof gn);
      screen_puts("  Owner  : "); screen_puts(un); screen_puts(" (uid "); screen_put_dec(st.uid); screen_puts("), group "); screen_puts(gn); screen_puts(" (gid "); screen_put_dec(st.gid); screen_puts(")\n"); }
    if (st.type == FS_DIR) { screen_puts("  Entries: "); screen_put_dec(st.children); screen_putc('\n'); }
    else { screen_puts("  Size   : "); screen_put_dec((uint32_t)st.size); screen_puts(" bytes\n"); }
    screen_puts("  Changed: tick "); screen_put_dec(st.mtime); screen_puts(" (about "); screen_put_dec(st.mtime / TIMER_HZ); screen_puts(" s after boot)\n");
    if (st.protect) screen_puts("  Flags  : protected (cannot be changed, moved or deleted until 'unprotect')\n");
    if (st.locked) { char ln[20]; cu_uid_name(st.lock_uid, ln, sizeof ln); screen_puts("  Lock   : held by "); screen_puts(ln); screen_puts(" (uid "); screen_put_dec(st.lock_uid); screen_puts(")\n"); }
    return 0;
}

static int cmd_file(int argc, const char **argv) {
    if (argc != 2) return cu_usage("file <path>");
    fs_stat_t st; int rc = sys_stat(argv[1], &st);
    if (rc != 0) return cu_fs_error("file", argv[1], rc, CU_FILE);
    screen_puts(argv[1]); screen_puts(": ");
    if (st.type == FS_DIR) { screen_puts("directory\n"); return 0; }
    if (st.size == 0) { screen_puts("empty\n"); return 0; }
    char buf[512];
    int fd = sys_open(argv[1], SYS_O_READ);
    if (fd < 0) return cu_fs_error("file", argv[1], fd, CU_FILE);
    int n = sys_read(fd, buf, sizeof buf);                      /* only the first 512 bytes decide */
    sys_close(fd);
    if (n < 0) return cu_fs_error("file", argv[1], n, CU_FILE);
    screen_puts(text_is_text(buf, (size_t)n) ? "ASCII text" : "binary data");
    screen_putc('\n');
    return 0;
}

static int cmd_truncate(int argc, const char **argv) {
    uint32_t n;
    if (argc != 3) return cu_usage("truncate <file> <size>");
    if (!cu_parse_count(argv[2], &n, FS_MAX_FILE_SIZE)) return cu_error("truncate", "size must be 0 to 262144");
    int rc = sys_truncate(argv[1], n);
    return rc == 0 ? 0 : cu_fs_error("truncate", argv[1], rc, CU_FILE);
}

static int cmd_mkfile(int argc, const char **argv) {
    uint32_t n;
    if (argc != 3) return cu_usage("mkfile <file> <size>   (new file filled with zero bytes)");
    if (!cu_parse_count(argv[2], &n, FS_MAX_FILE_SIZE)) return cu_error("mkfile", "size must be 0 to 262144");
    int rc = sys_create(argv[1]);
    if (rc != 0) return cu_fs_error("mkfile", argv[1], rc, CU_ANY);
    rc = sys_truncate(argv[1], n);
    if (rc != 0) { sys_delete(argv[1]); return cu_fs_error("mkfile", argv[1], rc, CU_ANY); }
    return 0;
}

static int cmd_exists(int argc, const char **argv) {
    if (argc != 2) return cu_usage("exists <path>");
    fs_stat_t probe; bool yes = sys_stat(argv[1], &probe) == 0;
    screen_puts(yes ? "yes\n" : "no\n");
    return yes ? 0 : 1;
}

static int cmd_filesize(int argc, const char **argv) {
    if (argc != 2) return cu_usage("filesize <file>");
    fs_stat_t st; int rc = sys_stat(argv[1], &st);
    if (rc != 0) return cu_fs_error("filesize", argv[1], rc, CU_FILE);
    if (st.type == FS_DIR) return cu_fs_error("filesize", argv[1], FS_ERR_IS_DIR, CU_FILE);
    screen_put_dec((uint32_t)st.size); screen_putc('\n');
    return 0;
}

/* readfile: reads with an explicit offset and length using open / lseek / read. */
static int cmd_readfile(int argc, const char **argv) {
    uint32_t off = 0, len = 64;
    if (argc < 2 || argc > 4) return cu_usage("readfile <file> [offset] [length<=256]");
    if (argc >= 3 && !cu_parse_count(argv[2], &off, FS_MAX_FILE_SIZE)) return cu_error("readfile", "bad offset");
    if (argc == 4 && (!cu_parse_count(argv[3], &len, 256) || len == 0)) return cu_error("readfile", "length must be 1 to 256");
    char buf[256];
    int fd = sys_open(argv[1], SYS_O_READ);
    if (fd < 0) return cu_fs_error("readfile", argv[1], fd, CU_FILE);
    int n = sys_lseek(fd, (int32_t)off, SYS_SEEK_SET);
    if (n >= 0) n = sys_read(fd, buf, len);
    sys_close(fd);
    if (n < 0) return cu_fs_error("readfile", argv[1], n, CU_FILE);
    screen_puts("read "); screen_put_dec((uint32_t)n); screen_puts(" byte(s) at offset "); screen_put_dec(off); screen_puts(":\n");
    cu_print_text(buf, (size_t)n); screen_putc('\n');
    return 0;
}

static int copy_to(const char *cmd, const char *from, const char *to) {
    int rc = fs_copy(from, to);
    return rc == FS_OK ? 0 : cu_fs_error(cmd, rc == FS_ERR_NOT_FOUND ? from : to, rc, CU_FILE);
}
static int cmd_backup(int argc, const char **argv) {
    if (argc != 2) return cu_usage("backup <file>   (copies it to <file>.bak)");
    char to[FS_PATH_MAX]; if (strlen(argv[1]) + 5 > sizeof to) return cu_error("backup", "path too long");
    memcpy(to, argv[1], strlen(argv[1])); memcpy(to + strlen(argv[1]), ".bak", 5);
    int rc = copy_to("backup", argv[1], to);
    if (rc == 0) { screen_puts("saved "); screen_puts(to); screen_putc('\n'); }
    return rc;
}
static int cmd_restore(int argc, const char **argv) {
    if (argc != 2) return cu_usage("restore <file>   (copies <file>.bak back over it)");
    size_t n = strlen(argv[1]); char orig[FS_PATH_MAX], bak[FS_PATH_MAX];
    if (n + 5 > sizeof bak) return cu_error("restore", "path too long");
    if (n > 4 && !strcmp(argv[1] + n - 4, ".bak")) { memcpy(orig, argv[1], n - 4); orig[n - 4] = '\0'; memcpy(bak, argv[1], n + 1); }
    else { memcpy(orig, argv[1], n + 1); memcpy(bak, argv[1], n); memcpy(bak + n, ".bak", 5); }
    int rc = copy_to("restore", bak, orig);
    if (rc == 0) { screen_puts("restored "); screen_puts(orig); screen_putc('\n'); }
    return rc;
}

static int cmd_swapfiles(int argc, const char **argv) {
    if (argc != 3) return cu_usage("swapfiles <file1> <file2>");
    int rc = fs_swap_data(argv[1], argv[2]);
    if (rc == FS_ERR_NOT_FOUND) { return cu_fs_error("swapfiles", fs_stat(argv[1], NULL) == FS_OK ? argv[2] : argv[1], rc, CU_FILE); }
    return rc == FS_OK ? 0 : cu_fs_error("swapfiles", argv[1], rc, CU_FILE);
}

static int cmd_fill(int argc, const char **argv) {
    uint32_t count;
    if (argc != 4) return cu_usage("fill <file> <pattern> <repeat-count>");
    size_t pl = strlen(argv[2]);
    if (pl == 0 || !cu_parse_count(argv[3], &count, FS_MAX_FILE_SIZE) || count == 0) return cu_error("fill", "repeat count must be 1 or more");
    if (pl * count > FS_MAX_FILE_SIZE) return cu_error("fill", "result would exceed the 256 KiB file limit");
    char *buf = sys_memory_alloc(pl * count);
    if (!buf) return cu_error("fill", "out of memory");
    for (uint32_t i = 0; i < count; i++) memcpy(buf + i * pl, argv[2], pl);
    int rc = put_file("fill", argv[1], buf, pl * count, false);
    sys_memory_free(buf);
    return rc;
}

static int cmd_touchn(int argc, const char **argv) {
    uint32_t n;
    if (argc != 3) return cu_usage("touchn <prefix> <count 1-100>   (creates prefix1 ... prefixN)");
    if (!cu_parse_count(argv[2], &n, 100) || n == 0) return cu_error("touchn", "count must be 1 to 100");
    size_t pl = strlen(argv[1]);
    if (pl + 4 > FS_PATH_MAX) return cu_error("touchn", "prefix too long");
    uint32_t made = 0;
    for (uint32_t i = 1; i <= n; i++) {
        char p[FS_PATH_MAX + 4]; memcpy(p, argv[1], pl);
        char d[6]; size_t k = 0, v = i; do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        size_t w = pl; while (k) p[w++] = d[--k]; p[w] = '\0';
        int rc = sys_create(p);
        if (rc != 0) { cu_fs_error("touchn", p, rc, CU_ANY); screen_puts("created "); screen_put_dec(made); screen_puts(" before the error\n"); return 1; }
        made++;
    }
    screen_puts("created "); screen_put_dec(made); screen_puts(" files\n");
    return 0;
}

/* ---------- trash ---------- */
static int cmd_trash(int argc, const char **argv) {
    if (argc < 2) return cu_usage("trash <file...>   (moves to " TRASH_DIR ")");
    int rc = sys_mkdir(TRASH_DIR, SYS_MKDIR_PARENTS);
    if (rc != 0) return cu_fs_error("trash", TRASH_DIR, rc, CU_ANY);
    int status = 0;
    for (int i = 1; i < argc; i++) {
        fs_stat_t st; rc = sys_stat(argv[i], &st);
        if (rc != 0) { status = cu_fs_error("trash", argv[i], rc, CU_FILE); continue; }
        char canon[FS_PATH_MAX], base[FS_NAME_MAX + 3], dest[FS_PATH_MAX];
        resolve(argv[i], canon, sizeof canon);
        size_t bl = strlen(fs_path_basename(canon)); if (bl > FS_NAME_MAX - 3) bl = FS_NAME_MAX - 3;
        memcpy(base, fs_path_basename(canon), bl); base[bl] = '\0';
        rc = SYS_EEXIST;
        for (int suffix = 0; suffix < 100 && rc == SYS_EEXIST; suffix++) {
            size_t n = sizeof(TRASH_DIR) - 1; memcpy(dest, TRASH_DIR "/", n + 1); n++;
            memcpy(dest + n, base, bl); n += bl;
            if (suffix) { dest[n++] = '.'; if (suffix >= 10) dest[n++] = (char)('0' + suffix / 10); dest[n++] = (char)('0' + suffix % 10); }
            dest[n] = '\0';
            rc = sys_rename(canon, dest);
        }
        if (rc != 0) status = cu_fs_error("trash", argv[i], rc, CU_ANY);
        else { screen_puts("moved "); screen_puts(canon); screen_puts(" to trash\n"); }
    }
    return status;
}

static int cmd_undelete(int argc, const char **argv) {
    if (argc != 2) return cu_usage("undelete <name-in-trash>   (restores into the working directory)");
    char from[FS_PATH_MAX]; size_t nl = strlen(argv[1]);
    if (nl + sizeof(TRASH_DIR) + 1 > sizeof from || strchr_slash(argv[1])) return cu_error("undelete", "give just the name shown by 'ls /tmp/.trash'");
    memcpy(from, TRASH_DIR "/", sizeof(TRASH_DIR)); memcpy(from + sizeof(TRASH_DIR), argv[1], nl + 1);
    int rc = sys_rename(from, argv[1]);
    if (rc == SYS_ENOENT) { fs_stat_t st; if (sys_stat(from, &st) != 0) return cu_error("undelete", "nothing with that name in the trash"); }
    if (rc != 0) return cu_fs_error("undelete", argv[1], rc, CU_ANY);
    screen_puts("restored "); screen_puts(argv[1]); screen_putc('\n');
    return 0;
}

static int cmd_emptytrash(int argc, const char **argv) {
    (void)argc; (void)argv;
    uint32_t n = 0;
    int rc = fs_remove_tree(TRASH_DIR, &n);
    if (rc == FS_ERR_NOT_FOUND) { screen_puts("trash is already empty\n"); return 0; }
    if (rc != FS_OK) return cu_fs_error("emptytrash", TRASH_DIR, rc, CU_ANY);
    screen_puts("deleted "); screen_put_dec(n > 0 ? n - 1 : 0); screen_puts(" item(s) from the trash\n");
    return 0;
}

/* shred: overwrite the contents with zero bytes, then delete. (Memory only: MiniFS has no disk.) */
static int cmd_shred(int argc, const char **argv) {
    if (argc != 2) return cu_usage("shred <file>");
    fs_stat_t st; int rc = sys_stat(argv[1], &st);
    if (rc != 0) return cu_fs_error("shred", argv[1], rc, CU_FILE);
    if (st.type == FS_DIR) return cu_fs_error("shred", argv[1], FS_ERR_IS_DIR, CU_FILE);
    if (st.size) {
        char *z = sys_memory_alloc(st.size);
        if (!z) return cu_error("shred", "out of memory");
        memset(z, 0, st.size);
        rc = put_file("shred", argv[1], z, st.size, false);
        sys_memory_free(z);
        if (rc != 0) return rc;
    }
    rc = sys_delete(argv[1]);
    if (rc != 0) return cu_fs_error("shred", argv[1], rc, CU_FILE);
    screen_puts("overwrote and deleted "); screen_puts(argv[1]); screen_puts(" (RAM only: MiniFS has no disk)\n");
    return 0;
}

static const struct { const char *name; cmd_handler_t fn; } bindings[] = {
    { "touch", cmd_touch }, { "write", cmd_write }, { "append", cmd_append }, { "note", cmd_note }, { "rm", cmd_rm },
    { "cp", cmd_cp }, { "mv", cmd_mv }, { "rename", cmd_rename }, { "stat", cmd_stat }, { "file", cmd_file },
    { "truncate", cmd_truncate }, { "mkfile", cmd_mkfile }, { "exists", cmd_exists }, { "filesize", cmd_filesize },
    { "readfile", cmd_readfile }, { "backup", cmd_backup }, { "restore", cmd_restore }, { "swapfiles", cmd_swapfiles },
    { "fill", cmd_fill }, { "touchn", cmd_touchn }, { "trash", cmd_trash }, { "undelete", cmd_undelete },
    { "emptytrash", cmd_emptytrash }, { "shred", cmd_shred },
};

int file_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
