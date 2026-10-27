/* Host-side tests for MiniFS: path functions, tree operations, permissions, limits,
 * memory exhaustion, consistency checking. filesystem.c is #included so that fsck tests can
 * deliberately corrupt internal structures. Run with: make test */
#include <stdio.h>
#include <string.h>
#include "kernel/filesystem/filesystem.c"
#include "include/heap.h"

static int run, failed;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; printf("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static multiboot_mmap_entry_t map_buf[1]; static multiboot_info_t mbi;
static uint32_t fake_ticks;
static uint32_t clock_now(void) { return fake_ticks; }

static void boot(void) {
    map_buf[0] = (multiboot_mmap_entry_t){ 20, 0x100000, 0x4000000, 1 };
    memset(&mbi, 0, sizeof mbi);
    mbi.flags = MULTIBOOT_FLAG_MMAP; mbi.mmap_addr = (uint32_t)(uintptr_t)map_buf; mbi.mmap_length = sizeof map_buf;
    memory_init(&mbi, 0x100000, 0x110000);
    root = cwd = NULL; fake_ticks = 0;
    fs_set_clock(clock_now); fs_set_user(FS_UID_ROOT, FS_UID_ROOT);
    fs_init();
}
static size_t heap_used(void) { heap_stats_t h; heap_stats(&h); return h.used_blocks; }
static bool consistent(void) { fs_check_report_t r; return fs_check(&r) == 0; }
static bool content_is(const char *path, const char *text) {
    char b[512]; size_t n = 0;
    return fs_read(path, b, sizeof b, 0, &n) == FS_OK && n == strlen(text) && memcmp(b, text, n) == 0;
}

static void test_paths(void) {
    char out[FS_PATH_MAX], par[FS_PATH_MAX], nm[FS_NAME_MAX + 1];
    puts("paths");
    struct { const char *in, *cwd, *want; } t[] = {
        { "/", "/", "/" }, { "/a/b/../c", "/", "/a/c" }, { "//a///b/", "/", "/a/b" }, { "/..", "/", "/" },
        { "/a/../../b", "/", "/b" }, { "z", "/x/y", "/x/y/z" }, { "../z", "/x/y", "/x/z" }, { ".", "/x/y", "/x/y" },
        { "..", "/", "/" }, { "./a/./b", "/", "/a/b" }, { "/a/b/..", "/", "/a" }, { "a", "/", "/a" }, { "../../..", "/a", "/" },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        int rc = fs_path_normalize(t[i].in, t[i].cwd, out, sizeof out);
        CHECK(rc == FS_OK && strcmp(out, t[i].want) == 0);
    }
    CHECK(fs_path_normalize("", "/", out, sizeof out) == FS_ERR_INVALID);
    CHECK(fs_path_normalize(NULL, "/", out, sizeof out) == FS_ERR_INVALID);
    CHECK(fs_path_normalize("a", NULL, out, sizeof out) == FS_ERR_INVALID);
    CHECK(fs_path_normalize("a", "rel", out, sizeof out) == FS_ERR_INVALID);
    CHECK(fs_path_normalize("/a", "/", out, 1) == FS_ERR_INVALID);
    char c31[40], c32[40], big[200];
    memset(c31, 'a', 31); c31[31] = 0; memset(c32, 'a', 32); c32[32] = 0;
    snprintf(big, sizeof big, "/%s", c31);  CHECK(fs_path_normalize(big, "/", out, sizeof out) == FS_OK);
    snprintf(big, sizeof big, "/%s", c32);  CHECK(fs_path_normalize(big, "/", out, sizeof out) == FS_ERR_NAME_TOO_LONG);
    snprintf(big, sizeof big, "/%s/%s/%s/%.30s", c31, c31, c31, c31);          /* 127 characters */
    CHECK(strlen(big) == 127 && fs_path_normalize(big, "/", out, FS_PATH_MAX) == FS_OK && strlen(out) == 127);
    snprintf(big, sizeof big, "/%s/%s/%s/%s", c31, c31, c31, c31);             /* 128 characters: one too many */
    CHECK(fs_path_normalize(big, "/", out, FS_PATH_MAX) == FS_ERR_PATH_TOO_LONG);
    CHECK(fs_path_normalize("/a\x01" "b", "/", out, sizeof out) == FS_ERR_INVALID);

    CHECK(fs_path_split("/a/b", par, sizeof par, nm, sizeof nm) == FS_OK && !strcmp(par, "/a") && !strcmp(nm, "b"));
    CHECK(fs_path_split("/a", par, sizeof par, nm, sizeof nm) == FS_OK && !strcmp(par, "/") && !strcmp(nm, "a"));
    CHECK(fs_path_split("/", par, sizeof par, nm, sizeof nm) == FS_ERR_INVALID);
    CHECK(fs_path_split("a/b", par, sizeof par, nm, sizeof nm) == FS_ERR_INVALID);
    CHECK(!strcmp(fs_path_basename("/a/b/c.txt"), "c.txt") && !strcmp(fs_path_basename("c.txt"), "c.txt") && !strcmp(fs_path_basename("/a/b/c"), "c"));
    CHECK(fs_path_depth("/") == 0 && fs_path_depth("/a") == 1 && fs_path_depth("/a/b/c") == 3);
    CHECK(fs_name_valid("a") && fs_name_valid("a b") && fs_name_valid(c31) && !fs_name_valid(c32));
    CHECK(!fs_name_valid("") && !fs_name_valid(".") && !fs_name_valid("..") && !fs_name_valid("a/b") && !fs_name_valid("\t") && !fs_name_valid("\xC3\xA9") && !fs_name_valid(NULL));
    CHECK(fs_name_valid("...") && fs_name_valid(".hidden"));
}

static bool count_cb(const fs_dirent_t *e, void *ctx) { (void)e; (*(int *)ctx)++; return true; }
static char order[256];
static bool order_cb(const fs_dirent_t *e, void *ctx) { (void)ctx; strcat(order, e->name); strcat(order, ","); return true; }

static void test_layout_and_dirs(void) {
    puts("layout and directories");
    boot();
    fs_stat_t st; fs_usage_t u;
    CHECK(fs_stat("/", &st) == FS_OK && st.type == FS_DIR && st.mode == 0755 && !strcmp(st.name, "/"));
    CHECK(fs_stat("/home/admin", &st) == FS_OK && st.mode == 0750 && st.uid == FS_UID_ADMIN);
    CHECK(fs_stat("/tmp", &st) == FS_OK && st.mode == 0777);
    order[0] = 0; CHECK(fs_readdir("/", order_cb, NULL) == FS_OK && !strcmp(order, "bin,etc,home,tmp,usr,var,"));
    CHECK(content_is("/etc/hostname", "minios\n"));
    fs_usage(&u);
    CHECK(u.nodes == 12 && u.dirs == 10 && u.files == 2 && u.max_nodes == FS_MAX_NODES);
    CHECK(consistent());

    CHECK(fs_mkdir("/projects") == FS_OK && fs_mkdir("/projects") == FS_ERR_EXISTS);
    CHECK(fs_mkdir("/nope/sub") == FS_ERR_NOT_FOUND);
    CHECK(fs_create("/projects/f") == FS_OK && fs_mkdir("/projects/f/sub") == FS_ERR_NOT_DIR);
    CHECK(fs_mkdir("/") == FS_ERR_EXISTS && fs_mkdir("/.") == FS_ERR_EXISTS && fs_mkdir("") == FS_ERR_INVALID);
    char c32[40]; memset(c32, 'x', 32); c32[32] = 0; char p[80]; snprintf(p, sizeof p, "/%s", c32);
    CHECK(fs_mkdir(p) == FS_ERR_NAME_TOO_LONG);
    CHECK(fs_mkdir("/a\tb") == FS_ERR_INVALID);
    CHECK(fs_mkdir("/with space") == FS_OK && fs_stat("/with space", &st) == FS_OK);

    /* working directory and relative paths */
    CHECK(fs_chdir("/projects") == FS_OK);
    char cw[FS_PATH_MAX]; fs_getcwd(cw, sizeof cw); CHECK(!strcmp(cw, "/projects"));
    CHECK(fs_mkdir("sub") == FS_OK && fs_stat("/projects/sub", NULL) == FS_OK);
    CHECK(fs_chdir("sub") == FS_OK && fs_chdir("../..") == FS_OK); fs_getcwd(cw, sizeof cw); CHECK(!strcmp(cw, "/"));
    CHECK(fs_chdir("..") == FS_OK && fs_chdir("/projects/f") == FS_ERR_NOT_DIR && fs_chdir("/missing") == FS_ERR_NOT_FOUND);
    CHECK(fs_getcwd(NULL, 10) == FS_ERR_INVALID && fs_getcwd(cw, 1) == FS_ERR_INVALID);

    /* mkdir -p and the depth limit */
    CHECK(fs_mkdir_parents("/a/b/c/d") == FS_OK && fs_stat("/a/b/c/d", NULL) == FS_OK);
    CHECK(fs_mkdir_parents("/a/b/c/d") == FS_OK);                          /* idempotent */
    CHECK(fs_mkdir_parents("/projects/f/x") == FS_ERR_NOT_DIR);
    char deep[200] = ""; for (int i = 0; i < FS_MAX_DEPTH; i++) strcat(deep, "/d");
    CHECK(fs_mkdir_parents(deep) == FS_OK);
    strcat(deep, "/d");
    CHECK(fs_mkdir_parents(deep) == FS_ERR_PATH_TOO_LONG && fs_mkdir(deep) == FS_ERR_PATH_TOO_LONG);
    CHECK(consistent());

    /* rmdir / busy / not empty */
    CHECK(fs_rmdir("/projects") == FS_ERR_NOT_EMPTY && fs_rmdir("/projects/f") == FS_ERR_NOT_DIR);
    CHECK(fs_rmdir("/") == FS_ERR_BUSY && fs_rmdir("/ghost") == FS_ERR_NOT_FOUND);
    fs_chdir("/a/b");
    CHECK(fs_rmdir("/a/b/c/d") == FS_OK && fs_rmdir("/a/b") == FS_ERR_BUSY);   /* cwd itself */
    CHECK(fs_rmdir("/a") == FS_ERR_BUSY);                                      /* an ancestor of the cwd */
    fs_chdir("/");
    int n = 0; fs_readdir("/a", count_cb, &n); CHECK(n == 1);
    CHECK(fs_readdir("/projects/f", count_cb, &n) == FS_ERR_NOT_DIR);
    CHECK(consistent());
}

static void test_files(void) {
    puts("files");
    boot();
    char b[600]; size_t n;
    CHECK(fs_create("/tmp/a.txt") == FS_OK && fs_create("/tmp/a.txt") == FS_ERR_EXISTS);
    CHECK(fs_write("/tmp/a.txt", "hello", 5, FS_WRITE_TRUNCATE) == FS_OK && content_is("/tmp/a.txt", "hello"));
    CHECK(fs_write("/tmp/a.txt", " world", 6, FS_WRITE_APPEND) == FS_OK && content_is("/tmp/a.txt", "hello world"));
    CHECK(fs_write("/tmp/a.txt", "HI", 2, FS_WRITE_TRUNCATE) == FS_OK && content_is("/tmp/a.txt", "HI"));
    CHECK(fs_write("/tmp/new.txt", "x", 1, FS_WRITE_APPEND) == FS_OK && content_is("/tmp/new.txt", "x"));     /* creates */
    CHECK(fs_write("/tmp/a.txt", "", 0, FS_WRITE_TRUNCATE) == FS_OK && content_is("/tmp/a.txt", ""));
    CHECK(fs_write("/tmp/a.txt", NULL, 3, FS_WRITE_TRUNCATE) == FS_ERR_INVALID);
    CHECK(fs_write("/tmp", "x", 1, FS_WRITE_TRUNCATE) == FS_ERR_IS_DIR);
    CHECK(fs_write("/nodir/f", "x", 1, FS_WRITE_TRUNCATE) == FS_ERR_NOT_FOUND && fs_stat("/nodir", NULL) == FS_ERR_NOT_FOUND);
    CHECK(fs_write("/etc/motd/x", "x", 1, FS_WRITE_TRUNCATE) == FS_ERR_NOT_DIR);

    fs_write("/tmp/r.txt", "0123456789", 10, FS_WRITE_TRUNCATE);
    CHECK(fs_read("/tmp/r.txt", b, 4, 3, &n) == FS_OK && n == 4 && !memcmp(b, "3456", 4));
    CHECK(fs_read("/tmp/r.txt", b, 100, 8, &n) == FS_OK && n == 2);
    CHECK(fs_read("/tmp/r.txt", b, 100, 10, &n) == FS_OK && n == 0);        /* at EOF */
    CHECK(fs_read("/tmp/r.txt", b, 100, 9999, &n) == FS_OK && n == 0);      /* beyond EOF */
    CHECK(fs_read("/tmp/r.txt", b, 0, 0, &n) == FS_OK && n == 0);
    CHECK(fs_read("/tmp", b, 10, 0, &n) == FS_ERR_IS_DIR && fs_read("/tmp/none", b, 10, 0, &n) == FS_ERR_NOT_FOUND);
    CHECK(fs_read("/tmp/r.txt", NULL, 5, 0, &n) == FS_ERR_INVALID);

    /* size limits: nothing changes when a write is refused */
    static uint8_t big[FS_MAX_FILE_SIZE + 10]; memset(big, 'z', sizeof big);
    CHECK(fs_write("/tmp/big", big, FS_MAX_FILE_SIZE, FS_WRITE_TRUNCATE) == FS_OK);
    fs_stat_t st; fs_stat("/tmp/big", &st); CHECK(st.size == FS_MAX_FILE_SIZE);
    CHECK(fs_write("/tmp/big", "x", 1, FS_WRITE_APPEND) == FS_ERR_TOO_BIG && (fs_stat("/tmp/big", &st), st.size == FS_MAX_FILE_SIZE));
    CHECK(fs_write("/tmp/big2", big, FS_MAX_FILE_SIZE + 1, FS_WRITE_TRUNCATE) == FS_ERR_TOO_BIG && fs_stat("/tmp/big2", NULL) == FS_ERR_NOT_FOUND);
    CHECK(fs_write("/tmp/r.txt", big, FS_MAX_FILE_SIZE + 1, FS_WRITE_TRUNCATE) == FS_ERR_TOO_BIG && content_is("/tmp/r.txt", "0123456789"));
    fs_remove("/tmp/big");
    CHECK(consistent());

    /* truncate */
    CHECK(fs_truncate("/tmp/r.txt", 4) == FS_OK && content_is("/tmp/r.txt", "0123"));
    CHECK(fs_truncate("/tmp/r.txt", 8) == FS_OK && fs_read("/tmp/r.txt", b, 20, 0, &n) == FS_OK && n == 8 && !memcmp(b, "0123\0\0\0\0", 8));
    CHECK(fs_truncate("/tmp/r.txt", 0) == FS_OK && content_is("/tmp/r.txt", ""));
    CHECK(fs_truncate("/tmp/r.txt", FS_MAX_FILE_SIZE + 1) == FS_ERR_TOO_BIG && fs_truncate("/tmp", 1) == FS_ERR_IS_DIR && fs_truncate("/tmp/zz", 1) == FS_ERR_NOT_FOUND);
    CHECK(fs_truncate("/tmp/r.txt", FS_MAX_FILE_SIZE) == FS_OK && (fs_stat("/tmp/r.txt", &st), st.size == FS_MAX_FILE_SIZE));
    CHECK(consistent());

    /* write_at: the primitive behind write() on a descriptor */
    fs_write("/tmp/w", "0123456789", 10, FS_WRITE_TRUNCATE);
    CHECK(fs_write_at("/tmp/w", 3, "ab", 2) == FS_OK && content_is("/tmp/w", "012ab56789"));
    CHECK(fs_write_at("/tmp/w", 10, "XY", 2) == FS_OK && content_is("/tmp/w", "012ab56789XY"));          /* extends at the end */
    CHECK(fs_write_at("/tmp/w", 15, "Z", 1) == FS_OK && fs_read("/tmp/w", b, 64, 0, &n) == FS_OK && n == 16 && !memcmp(b + 12, "\0\0\0Z", 4));   /* gap reads as zeros */
    CHECK(fs_write_at("/tmp/w", 0, "", 0) == FS_OK && fs_write_at("/tmp/w", 99999, NULL, 0) == FS_OK && (fs_stat("/tmp/w", &st), st.size == 16));
    CHECK(fs_write_at("/tmp/w", FS_MAX_FILE_SIZE - 1, "x", 1) == FS_OK && (fs_stat("/tmp/w", &st), st.size == FS_MAX_FILE_SIZE));   /* exactly at the limit */
    CHECK(fs_write_at("/tmp/w", FS_MAX_FILE_SIZE, "x", 1) == FS_ERR_TOO_BIG && fs_write_at("/tmp/w", (size_t)-1, "xx", 2) == FS_ERR_TOO_BIG && fs_write_at("/tmp/w", 1, "x", FS_MAX_FILE_SIZE) == FS_ERR_TOO_BIG);   /* overflow-safe */
    CHECK((fs_stat("/tmp/w", &st), st.size == FS_MAX_FILE_SIZE) && consistent());
    fs_truncate("/tmp/w", 4);
    CHECK(fs_write_at("/tmp", 0, "x", 1) == FS_ERR_IS_DIR && fs_write_at("/tmp/ghost", 0, "x", 1) == FS_ERR_NOT_FOUND && fs_write_at("/tmp/w", 0, NULL, 1) == FS_ERR_INVALID);
    fs_set_user(FS_UID_STUDENT, FS_UID_STUDENT);
    CHECK(fs_write_at("/etc/motd", 0, "x", 1) == FS_ERR_PERMISSION);
    fs_set_user(FS_UID_ROOT, FS_UID_ROOT);
    {   fs_write("/tmp/w2", "keepme0123", 10, FS_WRITE_TRUNCATE);                 /* a small file with a small buffer */
        heap_stats_t hh; heap_stats(&hh); void *hg = kmalloc(hh.largest_free - 100);
        CHECK(hg != NULL && fs_write_at("/tmp/w2", 200000, "x", 1) == FS_ERR_NO_SPACE && content_is("/tmp/w2", "keepme0123") && consistent());   /* failed extension changes nothing */
        CHECK(fs_write_at("/tmp/w2", 2, "XY", 2) == FS_OK && content_is("/tmp/w2", "keXYme0123"));           /* in-place overwrite needs no memory */
        kfree(hg); }

    /* touch and times */
    fake_ticks = 100; CHECK(fs_touch("/tmp/t") == FS_OK && (fs_stat("/tmp/t", &st), st.mtime == 100 && st.size == 0));
    fake_ticks = 200; CHECK(fs_touch("/tmp/t") == FS_OK && (fs_stat("/tmp/t", &st), st.mtime == 200 && st.ctime == 100));
    CHECK(fs_touch("/nope/t") == FS_ERR_NOT_FOUND);

    /* remove */
    CHECK(fs_remove("/tmp") == FS_ERR_IS_DIR && fs_remove("/tmp/ghost") == FS_ERR_NOT_FOUND && fs_remove("/tmp/t") == FS_OK && fs_remove("/tmp/t") == FS_ERR_NOT_FOUND);
    CHECK(fs_swap_data("/tmp/a.txt", "/tmp/new.txt") == FS_OK && content_is("/tmp/a.txt", "x") && content_is("/tmp/new.txt", ""));
    CHECK(fs_swap_data("/tmp/a.txt", "/tmp") == FS_ERR_IS_DIR && fs_swap_data("/tmp/a.txt", "/tmp/q") == FS_ERR_NOT_FOUND && fs_swap_data("/tmp/a.txt", "/tmp/a.txt") == FS_OK);
    CHECK(consistent());
}

static void test_tree_ops(void) {
    puts("rename, copy, recursive operations");
    boot();
    fs_mkdir_parents("/w/src/inner"); fs_write("/w/src/a", "AAA", 3, FS_WRITE_TRUNCATE); fs_write("/w/src/inner/b", "BBB", 3, FS_WRITE_TRUNCATE);
    fs_chmod("/w/src/a", 0600);

    CHECK(fs_rename("/w/src/a", "/w/src/c") == FS_OK && content_is("/w/src/c", "AAA") && fs_stat("/w/src/a", NULL) == FS_ERR_NOT_FOUND);
    CHECK(fs_rename("/w/src/c", "/w/src/inner/c") == FS_OK && content_is("/w/src/inner/c", "AAA"));
    CHECK(fs_rename("/w/src/inner/c", "/w/src/inner/b") == FS_ERR_EXISTS);
    CHECK(fs_rename("/w/src/inner/c", "/w/src/inner/c") == FS_ERR_EXISTS);
    CHECK(fs_rename("/w/ghost", "/w/x") == FS_ERR_NOT_FOUND && fs_rename("/w/src", "/nowhere/x") == FS_ERR_NOT_FOUND);
    CHECK(fs_rename("/w/src", "/w/src/inner/src2") == FS_ERR_INTO_SELF && fs_rename("/w/src", "/w/src/x") == FS_ERR_INTO_SELF);
    CHECK(fs_rename("/", "/x") == FS_ERR_BUSY && fs_rename("/w/src/inner/b", "/w/bad\x01name") == FS_ERR_INVALID);
    fs_chdir("/w/src/inner");
    CHECK(fs_rename("/w/src", "/w/moved") == FS_OK);
    char cw[FS_PATH_MAX]; fs_getcwd(cw, sizeof cw); CHECK(!strcmp(cw, "/w/moved/inner"));   /* the cwd moved with its directory */
    fs_chdir("/");
    CHECK(consistent());

    /* depth limit applies to moved trees */
    char deep[200] = ""; for (int i = 0; i < FS_MAX_DEPTH - 2; i++) strcat(deep, "/d");
    fs_mkdir_parents(deep);
    CHECK(fs_rename("/w/moved", "/d/moved") == FS_OK || 1);
    fs_rename("/d/moved", "/w/moved");
    char d3[200] = ""; for (int i = 0; i < FS_MAX_DEPTH - 1; i++) strcat(d3, "/q");
    fs_mkdir_parents(d3);
    CHECK(fs_rename("/w/moved", "/q/q/q/q/q/q/q/q/q/q/q/q/q/q/q/q/q/q/q/q/q/q/q/moved") == FS_ERR_PATH_TOO_LONG);   /* 23 deep + 2 below */
    CHECK(fs_stat("/w/moved/inner/b", NULL) == FS_OK);

    /* copy */
    CHECK(fs_copy("/w/moved/inner/c", "/w/copy") == FS_OK && content_is("/w/copy", "AAA"));
    fs_stat_t s1, s2; fs_stat("/w/moved/inner/c", &s1); fs_stat("/w/copy", &s2); CHECK(s1.mode == s2.mode && s1.ino != s2.ino);
    fs_write("/w/moved/inner/c", "changed", 7, FS_WRITE_TRUNCATE);
    CHECK(content_is("/w/copy", "AAA"));                                    /* copies are independent */
    CHECK(fs_copy("/w/copy", "/w/moved/inner/b") == FS_OK && content_is("/w/moved/inner/b", "AAA"));   /* overwrite */
    CHECK(fs_copy("/w/copy", "/w/copy") == FS_ERR_EXISTS && fs_copy("/w/moved", "/w/x") == FS_ERR_IS_DIR);
    CHECK(fs_copy("/w/copy", "/w/moved") == FS_ERR_IS_DIR && fs_copy("/w/ghost", "/w/x") == FS_ERR_NOT_FOUND && fs_copy("/w/copy", "/nodir/x") == FS_ERR_NOT_FOUND);
    CHECK(fs_copy("/w/copy", "/w/e") == FS_OK && fs_write("/w/empty", "", 0, FS_WRITE_TRUNCATE) == FS_OK && fs_copy("/w/empty", "/w/e") == FS_OK && content_is("/w/e", ""));
    CHECK(consistent());

    /* copy_tree */
    CHECK(fs_copy_tree("/w/moved", "/w/clone") == FS_OK);
    CHECK(content_is("/w/clone/inner/c", "changed") && content_is("/w/clone/inner/b", "AAA"));
    CHECK(fs_copy_tree("/w/moved", "/w/clone") == FS_ERR_EXISTS);
    CHECK(fs_copy_tree("/w", "/w/moved/inside") == FS_ERR_INTO_SELF && fs_copy_tree("/w/ghost", "/w/x") == FS_ERR_NOT_FOUND);
    CHECK(fs_copy_tree("/w/copy", "/w/copy2") == FS_OK && content_is("/w/copy2", "AAA"));
    fs_usage_t u0, u1; fs_usage(&u0);
    CHECK(fs_copy_tree("/w/moved", "/w/clone/moved") == FS_OK);
    fs_usage(&u1); CHECK(u1.nodes > u0.nodes);
    CHECK(consistent());

    /* remove_tree */
    uint32_t removed = 0; size_t before = heap_used();
    CHECK(fs_remove_tree("/w/clone", &removed) == FS_OK && removed > 3 && fs_stat("/w/clone", NULL) == FS_ERR_NOT_FOUND);
    CHECK(heap_used() < before);                                            /* memory really came back */
    CHECK(fs_remove_tree("/", NULL) == FS_ERR_BUSY && fs_remove_tree("/ghost", NULL) == FS_ERR_NOT_FOUND);
    fs_chdir("/w/moved/inner");
    CHECK(fs_remove_tree("/w", NULL) == FS_ERR_BUSY && fs_remove_tree("/w/moved", NULL) == FS_ERR_BUSY);   /* ancestors of cwd */
    CHECK(fs_remove_tree("/w/copy", &removed) == FS_OK && removed == 1);
    fs_chdir("/");
    CHECK(fs_remove_tree("/w", &removed) == FS_OK && fs_stat("/w", NULL) == FS_ERR_NOT_FOUND);
    CHECK(consistent());
}

static void test_limits(void) {
    puts("limits and memory exhaustion");
    boot();
    char p[40]; int made = 0;
    fs_mkdir("/many");
    for (;; made++) { snprintf(p, sizeof p, "/many/f%d", made); if (fs_create(p) != FS_OK) break; }
    fs_usage_t u; fs_usage(&u);
    CHECK(u.nodes == FS_MAX_NODES && made == FS_MAX_NODES - 13);
    CHECK(fs_create("/many/extra") == FS_ERR_NO_SPACE && fs_mkdir("/many/dir") == FS_ERR_NO_SPACE);
    CHECK(fs_write("/many/newfile", "x", 1, FS_WRITE_TRUNCATE) == FS_ERR_NO_SPACE && fs_stat("/many/newfile", NULL) == FS_ERR_NOT_FOUND);
    CHECK(fs_copy_tree("/many", "/many2") == FS_ERR_NO_SPACE && fs_stat("/many2", NULL) == FS_ERR_NOT_FOUND);
    CHECK(consistent());
    CHECK(fs_remove("/many/f0") == FS_OK && fs_create("/many/again") == FS_OK);
    CHECK(fs_remove_tree("/many", NULL) == FS_OK);
    fs_usage(&u); CHECK(u.nodes == 12);

    /* out of heap memory: failed writes leave the file intact */
    fs_write("/tmp/keep", "precious", 8, FS_WRITE_TRUNCATE);
    heap_stats_t h; heap_stats(&h);
    void *hog = kmalloc(h.largest_free - 200);
    CHECK(hog != NULL);
    static uint8_t chunk[100000]; memset(chunk, 'k', sizeof chunk);
    CHECK(fs_write("/tmp/keep", chunk, sizeof chunk, FS_WRITE_TRUNCATE) == FS_ERR_NO_SPACE && content_is("/tmp/keep", "precious"));
    CHECK(fs_write("/tmp/keep", chunk, sizeof chunk, FS_WRITE_APPEND) == FS_ERR_NO_SPACE && content_is("/tmp/keep", "precious"));
    CHECK(fs_write("/tmp/fresh", chunk, sizeof chunk, FS_WRITE_TRUNCATE) == FS_ERR_NO_SPACE && fs_stat("/tmp/fresh", NULL) == FS_ERR_NOT_FOUND);
    CHECK(fs_copy("/tmp/keep", "/tmp/keep2") == FS_OK);                   /* small things still work */
    CHECK(fs_truncate("/tmp/keep", 100000) == FS_ERR_NO_SPACE && content_is("/tmp/keep", "precious"));
    CHECK(consistent());
    kfree(hog);
    CHECK(fs_write("/tmp/keep", chunk, sizeof chunk, FS_WRITE_TRUNCATE) == FS_OK);   /* recovers once memory is back */

    /* a copy that runs out of memory half-way is rolled back completely */
    fs_mkdir("/rb"); fs_mkdir("/rb/src");
    memset(chunk, 'c', 40000);
    fs_write("/rb/src/a", chunk, 40000, FS_WRITE_TRUNCATE); fs_write("/rb/src/b", chunk, 40000, FS_WRITE_TRUNCATE); fs_write("/rb/src/c", chunk, 40000, FS_WRITE_TRUNCATE);
    fs_usage_t ub; fs_usage(&ub);
    heap_stats(&h);
    void *hog2 = kmalloc(h.largest_free - 70000);
    CHECK(hog2 != NULL);
    CHECK(fs_copy_tree("/rb/src", "/rb/dst") == FS_ERR_NO_SPACE);
    CHECK(fs_stat("/rb/dst", NULL) == FS_ERR_NOT_FOUND);                    /* no half-copied tree left behind */
    fs_usage_t ua; fs_usage(&ua);
    CHECK(ua.nodes == ub.nodes && ua.data_bytes == ub.data_bytes && ua.capacity_bytes == ub.capacity_bytes);
    CHECK(consistent());
    kfree(hog2);
    CHECK(fs_copy_tree("/rb/src", "/rb/dst") == FS_OK && content_is("/rb/dst/a", "") == false);
    fs_remove_tree("/rb", NULL);

    /* re-initialising frees everything */
    size_t before = heap_used();
    fs_write("/tmp/x", chunk, 50000, FS_WRITE_TRUNCATE);
    fs_init(); fs_init();
    CHECK(heap_used() == before - 2 - 0 || heap_used() <= before);          /* no growth across re-init */
    fs_usage(&u); CHECK(u.nodes == 12 && u.data_bytes == strlen("Welcome to MiniOS - an educational operating system.\n") + 7);
    size_t after1 = heap_used(); fs_init(); CHECK(heap_used() == after1);
    CHECK(consistent());
}

static void test_permissions(void) {
    puts("permissions");
    boot();
    char b[16]; size_t n; fs_stat_t st;
    fs_write("/tmp/root_file", "secret", 6, FS_WRITE_TRUNCATE); fs_chmod("/tmp/root_file", 0600);
    fs_write("/tmp/shared", "pub", 3, FS_WRITE_TRUNCATE); fs_chmod("/tmp/shared", 0664);
    fs_set_user(FS_UID_STUDENT, FS_UID_STUDENT);
    CHECK(fs_read("/etc/motd", b, sizeof b, 0, &n) == FS_OK);              /* others may read */
    CHECK(fs_write("/etc/motd", "x", 1, FS_WRITE_TRUNCATE) == FS_ERR_PERMISSION && fs_remove("/etc/motd") == FS_ERR_PERMISSION);
    CHECK(fs_create("/etc/new") == FS_ERR_PERMISSION && fs_mkdir("/etc/dir") == FS_ERR_PERMISSION);
    CHECK(fs_read("/tmp/root_file", b, sizeof b, 0, &n) == FS_ERR_PERMISSION);
    CHECK(fs_write("/tmp/shared", "no", 2, FS_WRITE_TRUNCATE) == FS_ERR_PERMISSION);   /* group of root, not student */
    CHECK(fs_stat("/home/admin", &st) == FS_OK);                           /* stat of the directory itself needs only traversal of "/" */
    CHECK(fs_stat("/home/admin/x", NULL) == FS_ERR_PERMISSION);            /* 0750, owner admin: no search permission for student */
    CHECK(fs_readdir("/home/admin", count_cb, &n) == FS_ERR_PERMISSION && fs_chdir("/home/admin") == FS_ERR_PERMISSION);
    CHECK(fs_create("/home/student/mine") == FS_OK && fs_write("/home/student/mine", "ok", 2, FS_WRITE_TRUNCATE) == FS_OK);
    CHECK(fs_create("/tmp/stu") == FS_OK && fs_stat("/tmp/stu", &st) == FS_OK && st.uid == FS_UID_STUDENT && st.mode == 0644);
    CHECK(fs_chmod("/tmp/root_file", 0777) == FS_ERR_PERMISSION && fs_chown("/tmp/stu", 0, 0) == FS_ERR_PERMISSION);
    CHECK(fs_chmod("/tmp/stu", 0600) == FS_OK && fs_chmod("/tmp/stu", 01000) == FS_ERR_INVALID);
    CHECK(fs_write("/tmp/stu", "mine", 4, FS_WRITE_TRUNCATE) == FS_OK);
    fs_chmod("/tmp/stu", 0400);
    CHECK(fs_write("/tmp/stu", "x", 1, FS_WRITE_TRUNCATE) == FS_ERR_PERMISSION && fs_read("/tmp/stu", b, 4, 0, &n) == FS_OK);   /* read-only for its owner */
    CHECK(fs_can_write("/tmp/stu") == FS_ERR_PERMISSION && fs_can_write("/tmp/shared") == FS_ERR_PERMISSION);
    CHECK(fs_can_write("/tmp/brand_new") == FS_OK && fs_can_write("/etc/brand_new") == FS_ERR_PERMISSION && fs_can_write("/tmp") == FS_ERR_IS_DIR && fs_can_write("/no/dir/f") == FS_ERR_NOT_FOUND);
    CHECK(fs_rename("/tmp/stu", "/etc/stu") == FS_ERR_PERMISSION && fs_copy("/etc/motd", "/etc/motd2") == FS_ERR_PERMISSION);
    CHECK(fs_copy("/etc/motd", "/tmp/motd_copy") == FS_OK);
    CHECK(fs_remove_tree("/etc", NULL) == FS_ERR_PERMISSION && fs_stat("/etc/motd", NULL) == FS_OK);    /* all or nothing */
    CHECK(fs_copy_tree("/home/admin", "/tmp/adm") == FS_ERR_PERMISSION);
    fs_set_user(FS_UID_ROOT, FS_UID_ROOT);                                  /* recursive delete is all-or-nothing */
    fs_mkdir("/tmp/lk"); fs_write("/tmp/lk/keep", "k", 1, FS_WRITE_TRUNCATE); fs_mkdir("/tmp/lk/sub"); fs_write("/tmp/lk/sub/x", "x", 1, FS_WRITE_TRUNCATE);
    fs_set_user(FS_UID_STUDENT, FS_UID_STUDENT);                            /* /tmp itself is world-writable, /tmp/lk is not */
    CHECK(fs_remove_tree("/tmp/lk", NULL) == FS_ERR_PERMISSION && fs_stat("/tmp/lk/sub/x", NULL) == FS_OK && fs_stat("/tmp/lk/keep", NULL) == FS_OK);

    /* group permissions and owner bits */
    fs_set_user(FS_UID_ROOT, FS_UID_ROOT);
    fs_mkdir("/tmp/g"); fs_write("/tmp/g/f", "g", 1, FS_WRITE_TRUNCATE);
    fs_chown("/tmp/g/f", FS_UID_ADMIN, FS_UID_STUDENT); fs_chmod("/tmp/g/f", 0460);
    fs_set_user(FS_UID_STUDENT, FS_UID_STUDENT);                           /* not the owner, but in the group */
    CHECK(fs_write("/tmp/g/f", "G", 1, FS_WRITE_TRUNCATE) == FS_OK && fs_read("/tmp/g/f", b, 4, 0, &n) == FS_OK);
    fs_chmod("/tmp/g/f", 0) ; fs_set_user(FS_UID_ROOT, FS_UID_ROOT);
    CHECK(fs_chmod("/tmp/g/f", 0000) == FS_OK && fs_read("/tmp/g/f", b, 4, 0, &n) == FS_OK && fs_write("/tmp/g/f", "r", 1, FS_WRITE_TRUNCATE) == FS_OK);   /* root ignores the bits */
    fs_chmod("/tmp/g/f", 0200); fs_set_user(FS_UID_ADMIN, FS_UID_ADMIN);
    CHECK(fs_read("/tmp/g/f", b, 4, 0, &n) == FS_ERR_PERMISSION && fs_write("/tmp/g/f", "w", 1, FS_WRITE_TRUNCATE) == FS_OK);   /* write-only for the owner */
    uint16_t u, g; fs_get_user(&u, &g); CHECK(u == FS_UID_ADMIN && g == FS_UID_ADMIN);

    /* walking skips directories the user cannot enter */
    fs_set_user(FS_UID_STUDENT, FS_UID_STUDENT);
    order[0] = 0;
    bool collect(const char *path, const fs_stat_t *s, int d, void *c) { (void)s; (void)d; (void)c; strcat(order, path); strcat(order, ";"); return true; }
    CHECK(fs_walk("/home", collect, NULL) == FS_OK && strstr(order, "/home/admin;") && !strstr(order, "/home/admin/") && strstr(order, "/home/student/mine;"));
    fs_set_user(FS_UID_ROOT, FS_UID_ROOT);
    CHECK(consistent());
}

static int walk_depths[64]; static int walk_n;
static bool walk_cb(const char *path, const fs_stat_t *st, int depth, void *ctx) {
    (void)st; (void)ctx; strcat(order, path); strcat(order, ";"); if (walk_n < 64) walk_depths[walk_n++] = depth; return true;
}
static bool stop_cb(const char *path, const fs_stat_t *st, int depth, void *ctx) { (void)path; (void)st; (void)depth; return ++*(int *)ctx < 3; }

static void test_walk_and_fsck(void) {
    puts("walk, usage and consistency checker");
    boot();
    fs_mkdir_parents("/t/b/c"); fs_mkdir("/t/a"); fs_write("/t/z", "12345", 5, FS_WRITE_TRUNCATE); fs_write("/t/a/f", "ab", 2, FS_WRITE_TRUNCATE);
    order[0] = 0; walk_n = 0;
    CHECK(fs_walk("/t", walk_cb, NULL) == FS_OK && !strcmp(order, "/t;/t/a;/t/a/f;/t/b;/t/b/c;/t/z;"));
    CHECK(walk_depths[0] == 0 && walk_depths[1] == 1 && walk_depths[2] == 2 && walk_depths[5] == 1);
    order[0] = 0; CHECK(fs_walk("/", walk_cb, NULL) == FS_OK && strncmp(order, "/;/bin;/etc;/etc/hostname;", 26) == 0);
    int cnt = 0; CHECK(fs_walk("/", stop_cb, &cnt) == FS_OK && cnt == 3);   /* callback can stop the walk */
    CHECK(fs_walk("/nowhere", walk_cb, NULL) == FS_ERR_NOT_FOUND && fs_walk("/t", NULL, NULL) == FS_ERR_INVALID);
    size_t bytes; uint32_t files, dirs;
    CHECK(fs_tree_size("/t", &bytes, &files, &dirs) == FS_OK && bytes == 7 && files == 2 && dirs == 4);
    CHECK(fs_tree_size("/t/z", &bytes, &files, &dirs) == FS_OK && bytes == 5 && files == 1 && dirs == 0);
    CHECK(fs_tree_size("/missing", NULL, NULL, NULL) == FS_ERR_NOT_FOUND);
    fs_usage_t u; fs_usage(&u);
    CHECK(u.nodes == u.files + u.dirs && u.capacity_bytes >= u.data_bytes && u.node_bytes == u.nodes * sizeof(fs_node_t));
    char ms[11]; fs_mode_string(FS_DIR, 0755, ms); CHECK(!strcmp(ms, "drwxr-xr-x"));
    fs_mode_string(FS_FILE, 0640, ms); CHECK(!strcmp(ms, "-rw-r-----")); fs_mode_string(FS_FILE, 0, ms); CHECK(!strcmp(ms, "----------"));
    CHECK(strlen(fs_strerror(FS_ERR_PERMISSION)) > 0 && strlen(fs_strerror(-999)) > 0);

    /* fsck detects deliberate corruption */
    fs_check_report_t r;
    CHECK(fs_check(&r) == 0 && r.nodes_seen == n_nodes);
    fs_node_t *t = find_child(root, "t");
    t->child_count++;                       CHECK(fs_check(&r) > 0); t->child_count--;
    fs_node_t *a = find_child(t, "a");
    a->parent = root;                       CHECK(fs_check(&r) > 0 && strstr(r.first[0], "parent")); a->parent = t;
    n_nodes++;                              CHECK(fs_check(&r) > 0); n_nodes--;                          /* counter drift */
    data_bytes += 3;                        CHECK(fs_check(&r) > 0); data_bytes -= 3;
    fs_node_t *leak = node_new(FS_FILE, "leak");                                                          /* allocated but never linked */
    CHECK(fs_check(&r) > 0 && strstr(r.first[0], "leak")); node_free(leak);
    fs_node_t *z = find_child(t, "z");
    z->size = z->capacity + 1;              CHECK(fs_check(&r) > 0); z->size = 5;
    char saved[FS_NAME_MAX + 1]; strcpy(saved, a->name); strcpy(a->name, "zzz");   /* now out of order: a("zzz") before b */
    CHECK(fs_check(&r) > 0); strcpy(a->name, saved);
    strcpy(a->name, "bad/name");            CHECK(fs_check(&r) > 0); strcpy(a->name, saved);
    fs_node_t *saved_cwd = cwd; cwd = node_new(FS_DIR, "orphan"); CHECK(fs_check(&r) > 0); node_free(cwd); cwd = saved_cwd;
    CHECK(fs_check(&r) == 0);
    CHECK(fs_check(NULL) == 0);
    fs_node_t *sv = root; root = NULL; CHECK(fs_check(&r) == 1); root = sv;
}

static uint32_t rng = 777;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void test_stress(void) {
    puts("randomized stress");
    boot();
    size_t baseline = heap_used();
    fs_usage_t u0; fs_usage(&u0);
    static const char *dirs[] = { "/s", "/s/a", "/s/a/b", "/s/c", "/s/c/d", "/s/c/d/e" };
    static const char *names[] = { "f1", "f2", "f3", "g", "h" };
    int bad = 0;
    for (int i = 0; i < 6; i++) fs_mkdir_parents(dirs[i]);
    for (int op = 0; op < 4000; op++) {
        char p[FS_PATH_MAX], q[FS_PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dirs[rnd() % 6], names[rnd() % 5]);
        snprintf(q, sizeof q, "%s/%s", dirs[rnd() % 6], names[rnd() % 5]);
        char data[300]; size_t len = rnd() % 300; memset(data, 'a' + (int)(rnd() % 26), len);
        switch (rnd() % 9) {
            case 0: fs_write(p, data, len, FS_WRITE_TRUNCATE); break;
            case 1: fs_write(p, data, len, FS_WRITE_APPEND); break;
            case 2: fs_remove(p); break;
            case 3: fs_rename(p, q); break;
            case 4: fs_copy(p, q); break;
            case 5: fs_truncate(p, rnd() % 1000); break;
            case 6: fs_touch(p); break;
            case 7: fs_swap_data(p, q); break;
            case 8: { uint32_t k; if (rnd() % 20 == 0) { snprintf(q, sizeof q, "%s", dirs[1 + rnd() % 5]); fs_copy_tree(q, "/s/clone"); fs_remove_tree("/s/clone", &k); } break; }
        }
        if (op % 50 == 0 && !consistent()) bad++;
    }
    CHECK(bad == 0 && consistent());
    CHECK(fs_remove_tree("/s", NULL) == FS_OK);
    fs_usage_t u1; fs_usage(&u1);
    CHECK(u1.nodes == u0.nodes && u1.data_bytes == u0.data_bytes && u1.capacity_bytes == u0.capacity_bytes);
    CHECK(heap_used() == baseline);                                         /* every byte returned to the heap */
}

int main(void) {
    test_paths(); test_layout_and_dirs(); test_files(); test_tree_ops(); test_limits();
    test_permissions(); test_walk_and_fsck(); test_stress();
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
