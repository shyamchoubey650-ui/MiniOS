/* Directory commands (Phase 8, moved onto system calls in Phase 9): pwd ls cd mkdir rmdir tree find pushd popd dirs home up dircount
 * lsdirs lsfiles lsr mkpath rmtree cpdir isdir. They call the MiniFS API directly; Phase 9 moves
 * them behind system calls. */
#include "include/builtins.h"
#include "include/cmdutil.h"
#include "include/screen.h"
#include "include/filesystem.h"
#include "include/syscall.h"
#include "include/glob.h"
#include "include/string.h"

#define HOME_DIR "/home/admin"            /* replaced by the logged-in user's home in Phase 10 */

/* ---------- pwd / cd / home / up / pushd / popd / dirs ---------- */
static int cmd_pwd(int argc, const char **argv) {
    (void)argc; (void)argv;
    char p[FS_PATH_MAX];
    int rc = sys_getcwd(p, sizeof p);
    if (rc < 0) return cu_error("pwd", sys_strerror(rc));
    screen_puts(p); screen_putc('\n');
    return 0;
}

static int do_cd(const char *cmd, const char *path) {
    int rc = sys_chdir(path);
    return rc == 0 ? 0 : cu_fs_error(cmd, path, rc, CU_DIR);
}
static int cmd_cd(int argc, const char **argv) {
    if (argc > 2) return cu_usage("cd [directory]");
    return do_cd("cd", argc == 2 ? argv[1] : HOME_DIR);
}
static int cmd_home(int argc, const char **argv) { (void)argc; (void)argv; return do_cd("home", HOME_DIR); }
static int cmd_up(int argc, const char **argv)   { (void)argc; (void)argv; return do_cd("up", ".."); }

#define STACK_MAX 8
static char dir_stack[STACK_MAX][FS_PATH_MAX];
static int  dir_top;

static void print_stack(void) {
    char p[FS_PATH_MAX]; sys_getcwd(p, sizeof p);
    screen_puts(p);
    for (int i = dir_top - 1; i >= 0; i--) { screen_putc(' '); screen_puts(dir_stack[i]); }
    screen_putc('\n');
}
static int cmd_pushd(int argc, const char **argv) {
    if (argc != 2) return cu_usage("pushd <directory>");
    if (dir_top >= STACK_MAX) return cu_error("pushd", "directory stack is full (8 entries)");
    char here[FS_PATH_MAX]; sys_getcwd(here, sizeof here);
    int rc = sys_chdir(argv[1]);
    if (rc != 0) return cu_fs_error("pushd", argv[1], rc, CU_DIR);
    memcpy(dir_stack[dir_top++], here, strlen(here) + 1);
    print_stack();
    return 0;
}
static int cmd_popd(int argc, const char **argv) {
    (void)argc; (void)argv;
    if (dir_top == 0) return cu_error("popd", "directory stack is empty");
    int rc = sys_chdir(dir_stack[dir_top - 1]);
    if (rc != 0) return cu_fs_error("popd", dir_stack[dir_top - 1], rc, CU_DIR);   /* entry stays on the stack */
    dir_top--;
    print_stack();
    return 0;
}
static int cmd_dirs(int argc, const char **argv) { (void)argc; (void)argv; print_stack(); return 0; }

/* ---------- ls family ---------- */
typedef struct { bool dirs, files, longfmt, all; uint32_t shown; int col; } list_ctx_t;

static void print_entry(const fs_dirent_t *e, list_ctx_t *c) {
    if ((e->type == FS_DIR && !c->dirs) || (e->type == FS_FILE && !c->files)) return;
    if (!c->all && e->name[0] == '.') return;
    c->shown++;
    if (c->longfmt) {
        char m[11]; fs_mode_string(e->type, e->mode, m);
        screen_puts(m); screen_putc(' ');
        cu_num_pad(e->type == FS_DIR ? 0 : (uint32_t)e->size, 7); screen_putc(' ');
        if (e->type == FS_DIR) { cu_color(VGA_LIGHT_CYAN); screen_puts(e->name); screen_putc('/'); cu_color(VGA_LIGHT_GREY); }
        else screen_puts(e->name);
        screen_putc('\n');
        return;
    }
    size_t w = strlen(e->name) + 3;
    if (c->col + (int)w > 78) { screen_putc('\n'); c->col = 0; }
    if (e->type == FS_DIR) { cu_color(VGA_LIGHT_CYAN); screen_puts(e->name); screen_putc('/'); cu_color(VGA_LIGHT_GREY); }
    else screen_puts(e->name);
    screen_puts("  "); c->col += (int)w;
}

static int list_dir(const char *cmd, int argc, const char **argv, bool dirs, bool files) {
    list_ctx_t c = { dirs, files, false, false, 0, 0 };
    const char *path = ".";
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (const char *f = argv[i] + 1; *f; f++) {
                if (*f == 'l') c.longfmt = true; else if (*f == 'a') c.all = true;
                else { char m[24] = "unknown option -"; m[16] = *f; m[17] = 0; return cu_error(cmd, m); }
            }
        } else if (path[0] == '.' && !path[1]) path = argv[i];
        else return cu_usage("ls [-l] [-a] [path]");
    }
    fs_stat_t st;
    int rc = sys_stat(path, &st);
    if (rc != 0) return cu_fs_error(cmd, path, rc, CU_DIR);
    if (st.type == FS_FILE) {                               /* ls on a file prints that file */
        fs_dirent_t e; memcpy(e.name, st.name, strlen(st.name) + 1); e.type = FS_FILE; e.size = st.size; e.mode = st.mode; e.ino = st.ino;
        print_entry(&e, &c);
        if (!c.longfmt) screen_putc('\n');
        return 0;
    }
    int total = sys_readdir(path, NULL, 0);                 /* first ask how many entries there are ... */
    if (total < 0) return cu_fs_error(cmd, path, total, CU_DIR);
    if (total > 0) {
        fs_dirent_t *list = sys_memory_alloc((size_t)total * sizeof *list);   /* ... then get them */
        if (!list) return cu_error(cmd, "out of memory");
        int n = sys_readdir(path, list, (uint32_t)total);
        if (n < 0) { sys_memory_free(list); return cu_fs_error(cmd, path, n, CU_DIR); }
        for (int i = 0; i < n; i++) print_entry(&list[i], &c);
        sys_memory_free(list);
    }
    if (!c.longfmt && c.shown) screen_putc('\n');
    if (c.shown == 0) screen_puts("(empty)\n");
    return 0;
}
static int cmd_ls(int argc, const char **argv)      { return list_dir("ls", argc, argv, true, true); }
static int cmd_lsdirs(int argc, const char **argv)  { return list_dir("lsdirs", argc, argv, true, false); }
static int cmd_lsfiles(int argc, const char **argv) { return list_dir("lsfiles", argc, argv, false, true); }

static int cmd_dircount(int argc, const char **argv) {
    if (argc > 2) return cu_usage("dircount [directory]");
    const char *p = argc == 2 ? argv[1] : ".";
    int n = sys_readdir(p, NULL, 0);
    if (n < 0) return cu_fs_error("dircount", p, n, CU_DIR);
    screen_put_dec((uint32_t)n); screen_puts(n == 1 ? " entry\n" : " entries\n");
    return 0;
}

static bool lsr_cb(const char *path, const fs_stat_t *st, int depth, void *c) {
    (void)depth; (*(uint32_t *)c)++;
    screen_puts(path); if (st->type == FS_DIR && path[1]) screen_putc('/'); screen_putc('\n');
    return true;
}
static int cmd_lsr(int argc, const char **argv) {
    if (argc > 2) return cu_usage("lsr [directory]");
    const char *p = argc == 2 ? argv[1] : "."; uint32_t n = 0;
    int rc = fs_walk(p, lsr_cb, &n);
    return rc == FS_OK ? 0 : cu_fs_error("lsr", p, rc, CU_DIR);
}

/* ---------- tree ---------- */
static bool tree_cb(const char *path, const fs_stat_t *st, int depth, void *c) {
    uint32_t *counts = c;
    if (depth == 0) { screen_puts(path); screen_putc('\n'); return true; }
    for (int i = 1; i < depth; i++) screen_puts("|   ");
    screen_puts("|-- ");
    if (st->type == FS_DIR) { counts[0]++; cu_color(VGA_LIGHT_CYAN); screen_puts(st->name); screen_putc('/'); cu_color(VGA_LIGHT_GREY); }
    else { counts[1]++; screen_puts(st->name); }
    screen_putc('\n');
    return true;
}
static int cmd_tree(int argc, const char **argv) {
    if (argc > 2) return cu_usage("tree [directory]");
    const char *p = argc == 2 ? argv[1] : "."; uint32_t counts[2] = { 0, 0 };
    fs_stat_t st; int rc = sys_stat(p, &st);
    if (rc != 0) return cu_fs_error("tree", p, rc, CU_DIR);
    if (st.type != FS_DIR) return cu_fs_error("tree", p, FS_ERR_NOT_DIR, CU_DIR);
    fs_walk(p, tree_cb, counts);
    screen_put_dec(counts[0]); screen_puts(counts[0] == 1 ? " directory, " : " directories, ");
    screen_put_dec(counts[1]); screen_puts(counts[1] == 1 ? " file\n" : " files\n");
    return 0;
}

/* ---------- find ---------- */
typedef struct { const char *pattern; uint32_t found; } find_ctx_t;
static bool find_cb(const char *path, const fs_stat_t *st, int depth, void *vc) {
    find_ctx_t *c = vc;
    if (depth > 0 && glob_match(c->pattern, st->name)) { c->found++; screen_puts(path); if (st->type == FS_DIR) screen_putc('/'); screen_putc('\n'); }
    return true;
}
static int cmd_find(int argc, const char **argv) {
    if (argc < 2 || argc > 3) return cu_usage("find <name-pattern> [directory]   (* and ? wildcards)");
    const char *start = argc == 3 ? argv[2] : ".";
    find_ctx_t c = { argv[1], 0 };
    int rc = fs_walk(start, find_cb, &c);
    if (rc != FS_OK) return cu_fs_error("find", start, rc, CU_DIR);
    if (c.found == 0) { screen_puts("no match for '"); screen_puts(argv[1]); screen_puts("'\n"); return 1; }
    return 0;
}

/* ---------- mkdir / mkpath / rmdir / rmtree / cpdir / isdir ---------- */
static int cmd_mkdir(int argc, const char **argv) {
    bool parents = false; int first = 1, status = 0;
    if (argc >= 2 && !strcmp(argv[1], "-p")) { parents = true; first = 2; }
    if (first >= argc) return cu_usage("mkdir [-p] <directory...>");
    for (int i = first; i < argc; i++) {
        int rc = sys_mkdir(argv[i], parents ? SYS_MKDIR_PARENTS : 0);
        if (rc != 0) status = cu_fs_error("mkdir", argv[i], rc, rc == FS_ERR_NOT_FOUND ? CU_DIR : CU_ANY);
    }
    return status;
}
static int cmd_mkpath(int argc, const char **argv) {
    if (argc < 2) return cu_usage("mkpath <path...>");
    int status = 0;
    for (int i = 1; i < argc; i++) { int rc = sys_mkdir(argv[i], SYS_MKDIR_PARENTS); if (rc != 0) status = cu_fs_error("mkpath", argv[i], rc, CU_ANY); }
    return status;
}
static int cmd_rmdir(int argc, const char **argv) {
    if (argc < 2) return cu_usage("rmdir <directory...>");
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int rc = sys_rmdir(argv[i]);
        if (rc == SYS_EBUSY) status = cu_error("rmdir", "cannot remove the root or a directory you are inside");
        else if (rc != 0) status = cu_fs_error("rmdir", argv[i], rc, CU_DIR);
    }
    return status;
}
static int cmd_rmtree(int argc, const char **argv) {
    if (argc != 2) return cu_usage("rmtree <path>   (deletes everything below it)");
    uint32_t n = 0; int rc = fs_remove_tree(argv[1], &n);
    if (rc == FS_ERR_BUSY) return cu_error("rmtree", "refusing: that is the root or contains your working directory");
    if (rc != FS_OK) return cu_fs_error("rmtree", argv[1], rc, CU_ANY);
    screen_puts("removed "); screen_put_dec(n); screen_puts(n == 1 ? " item\n" : " items\n");
    return 0;
}
static int cmd_cpdir(int argc, const char **argv) {
    if (argc != 3) return cu_usage("cpdir <source-dir> <new-dir>");
    fs_stat_t st; int rc = sys_stat(argv[1], &st);
    if (rc != 0) return cu_fs_error("cpdir", argv[1], rc, CU_DIR);
    if (st.type != FS_DIR) return cu_fs_error("cpdir", argv[1], FS_ERR_NOT_DIR, CU_DIR);
    rc = fs_copy_tree(argv[1], argv[2]);
    if (rc == FS_ERR_INTO_SELF) return cu_error("cpdir", "cannot copy a directory into itself");
    if (rc != FS_OK) return cu_fs_error("cpdir", argv[2], rc, CU_ANY);
    uint32_t files, dirs; fs_tree_size(argv[2], NULL, &files, &dirs);
    screen_puts("copied "); screen_put_dec(files); screen_puts(" file(s) in "); screen_put_dec(dirs); screen_puts(" directories\n");
    return 0;
}
static int cmd_isdir(int argc, const char **argv) {
    if (argc != 2) return cu_usage("isdir <path>");
    fs_stat_t st; bool yes = sys_stat(argv[1], &st) == 0 && st.type == FS_DIR;
    screen_puts(yes ? "yes\n" : "no\n");
    return yes ? 0 : 1;
}

static const struct { const char *name; cmd_handler_t fn; } bindings[] = {
    { "pwd", cmd_pwd }, { "cd", cmd_cd }, { "home", cmd_home }, { "up", cmd_up }, { "pushd", cmd_pushd },
    { "popd", cmd_popd }, { "dirs", cmd_dirs }, { "ls", cmd_ls }, { "lsdirs", cmd_lsdirs }, { "lsfiles", cmd_lsfiles },
    { "dircount", cmd_dircount }, { "lsr", cmd_lsr }, { "tree", cmd_tree }, { "find", cmd_find }, { "mkdir", cmd_mkdir },
    { "mkpath", cmd_mkpath }, { "rmdir", cmd_rmdir }, { "rmtree", cmd_rmtree }, { "cpdir", cmd_cpdir }, { "isdir", cmd_isdir },
};

int directory_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
