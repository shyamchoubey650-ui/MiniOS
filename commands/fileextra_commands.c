/* More file commands (Phase 11): more ln newer diff protect unprotect.
 * ln makes a COPY (MiniFS has no hard or symbolic links). diff compares line by line without aligning insertions. */
#include "include/builtins.h"
#include "include/cmdutil.h"
#include "include/screen.h"
#include "include/syscall.h"
#include "include/numparse.h"
#include "include/string.h"

#define PAGE_LINES 20

static int cmd_more(int argc, const char **argv) {
    if (argc != 2) return cu_usage("more <file>     (Enter = next page, q = quit)");
    char *b; size_t len;
    if (!cu_load_file("more", argv[1], &b, &len)) return 1;
    uint32_t shown = 0;
    for (size_t i = 0; i < len; i++) {
        char c = b[i];
        screen_putc((c >= 32 && c < 127) || c == '\n' ? c : '.');
        if (c == '\n' && ++shown % PAGE_LINES == 0 && i + 1 < len) {
            cu_color(VGA_YELLOW); screen_puts("-- more (Enter = next page, q = quit) --"); cu_color(VGA_LIGHT_GREY);
            char k; do { if (sys_read(0, &k, 1) != 1) k = 'q'; } while (k != '\n' && k != '\r' && k != 'q' && k != 'Q' && k != 3);
            screen_putc('\n');
            if (k == 'q' || k == 'Q' || k == 3) break;
        }
    }
    if (len && b[len - 1] != '\n') screen_putc('\n');
    cu_free_file(b);
    return 0;
}

static int cmd_ln(int argc, const char **argv) {
    int a = 1;
    if (argc >= 2 && !strcmp(argv[1], "-s")) a = 2;
    if (argc - a != 2) return cu_usage("ln [-s] <source> <name>     (MiniFS has no links: this makes a COPY)");
    const command_t *cp = command_find("cp");
    if (!cp || !cp->handler) return cu_error("ln", "cp is not available.");
    const char *args[3] = { "cp", argv[a], argv[a + 1] };
    int rc = cp->handler(3, args);
    if (rc == 0) screen_puts("(a copy was made: later changes to one file do not affect the other)\n");
    return rc;
}

static int cmd_newer(int argc, const char **argv) {
    fs_stat_t x, y;
    if (argc != 3) return cu_usage("newer <a> <b>     (exit status 0 if a was modified more recently than b; prints nothing)");
    int rc = sys_stat(argv[1], &x);
    if (rc != 0) return cu_fs_error("newer", argv[1], rc, CU_ANY);
    rc = sys_stat(argv[2], &y);
    if (rc != 0) return cu_fs_error("newer", argv[2], rc, CU_ANY);
    return x.mtime > y.mtime ? 0 : 1;
}

static const char *line_at(const char *b, size_t len, size_t index, size_t *l) {        /* index-th line, no newline */
    size_t pos = 0, n = 0;
    while (pos < len) {
        size_t e = pos; while (e < len && b[e] != '\n') e++;
        if (n == index) { *l = e - pos; if (*l && b[pos + *l - 1] == '\r') (*l)--; return b + pos; }
        n++; pos = e + 1;
    }
    return NULL;
}
static size_t count_lines(const char *b, size_t len) { size_t n = 0; for (size_t i = 0; i < len; i++) if (b[i] == '\n') n++; return n + ((len && b[len - 1] != '\n') ? 1 : 0); }

static int cmd_diff(int argc, const char **argv) {
    char *a, *b; size_t la, lb;
    if (argc != 3) return cu_usage("diff <file1> <file2>     (compares line N with line N; exit 0 if identical)");
    if (!cu_load_file("diff", argv[1], &a, &la)) return 1;
    if (!cu_load_file("diff", argv[2], &b, &lb)) { cu_free_file(a); return 1; }
    size_t na = count_lines(a, la), nb = count_lines(b, lb), n = na > nb ? na : nb, diffs = 0;
    for (size_t i = 0; i < n; i++) {
        size_t x = 0, y = 0; const char *p = i < na ? line_at(a, la, i, &x) : NULL, *q = i < nb ? line_at(b, lb, i, &y) : NULL;
        if (p && q && x == y && !memcmp(p, q, x)) continue;
        diffs++;
        if (diffs <= 20) {
            screen_put_dec((uint32_t)i + 1); screen_puts(":\n");
            if (p) { screen_puts("  < "); for (size_t k = 0; k < x; k++) screen_putc(p[k]); screen_putc('\n'); }
            if (q) { screen_puts("  > "); for (size_t k = 0; k < y; k++) screen_putc(q[k]); screen_putc('\n'); }
        }
    }
    if (diffs == 0) screen_puts("Files are identical.\n");
    else { if (diffs > 20) screen_puts("...\n"); screen_put_dec((uint32_t)diffs); screen_puts(" line(s) differ (compared position by position, so one inserted line shifts everything after it)\n"); }
    cu_free_file(a); cu_free_file(b);
    return diffs ? 1 : 0;
}

static int protect_cmd(const char *cmd, bool on, int argc, const char **argv) {
    if (argc < 2) { char u[40]; size_t n = strlen(cmd); memcpy(u, cmd, n); memcpy(u + n, " <path...>", 11); return cu_usage(u); }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int rc = sys_protect(argv[i], on);
        if (rc != 0) { status = cu_fs_error(cmd, argv[i], rc, CU_ANY); continue; }
        screen_puts(argv[i]); screen_puts(on ? " is protected: it cannot be changed, moved or deleted until 'unprotect'\n" : " is no longer protected\n");
    }
    return status;
}
static int cmd_protect(int argc, const char **argv) { return protect_cmd("protect", true, argc, argv); }
static int cmd_unprotect(int argc, const char **argv) { return protect_cmd("unprotect", false, argc, argv); }

/* ---------- walking the tree (lock -l, quota) ---------- */
typedef void (*walk_fn)(const char *path, const fs_stat_t *st, void *ctx);
static void walk_dir(const char *dir, int depth, uint32_t *budget, walk_fn fn, void *ctx) {
    if (depth > 10 || *budget == 0) return;
    int total = sys_readdir(dir, NULL, 0);
    if (total <= 0) return;                                              /* empty, or a directory we may not read */
    fs_dirent_t *list = sys_memory_alloc(sizeof(fs_dirent_t) * (size_t)total);
    if (!list) return;
    int n = sys_readdir(dir, list, (uint32_t)total);
    for (int i = 0; i < n && *budget; i++) {
        char path[FS_PATH_MAX]; size_t d = strlen(dir), k = strlen(list[i].name);
        if (d + 1 + k + 1 >= sizeof path) continue;
        memcpy(path, dir, d); if (d == 0 || path[d - 1] != '/') path[d++] = '/';
        memcpy(path + d, list[i].name, k + 1);
        fs_stat_t st;
        if (sys_stat(path, &st) != 0) continue;
        (*budget)--;
        fn(path, &st, ctx);
        if (st.type == FS_DIR) walk_dir(path, depth + 1, budget, fn, ctx);
    }
    sys_memory_free(list);
}

/* ---------- lock / unlock ---------- */
static void list_lock_cb(const char *path, const fs_stat_t *st, void *ctx) {
    if (!st->locked) return;
    char who[20]; cu_uid_name(st->lock_uid, who, sizeof who);
    screen_puts(path); screen_puts("   (locked by "); screen_puts(who); screen_puts(")\n");
    (*(uint32_t *)ctx)++;
}
static int cmd_lock(int argc, const char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "-l")) {
        if (argc > 3) return cu_usage("lock -l [directory]");
        const char *dir = argc == 3 ? argv[2] : "/"; uint32_t found = 0, budget = 4000;
        walk_dir(dir, 0, &budget, list_lock_cb, &found);
        if (!found) screen_puts("no locked files\n");
        return 0;
    }
    if (argc < 2) return cu_usage("lock <file...>   |   lock -l [directory]      (advisory: other users cannot change, move or delete it; you and root can; not saved by savefs)");
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int rc = sys_lock(argv[i], true);
        if (rc != 0) { status = cu_fs_error("lock", argv[i], rc, CU_FILE); continue; }
        screen_puts(argv[i]); screen_puts(" is locked: other users cannot change, move or delete it until 'unlock'\n");
    }
    return status;
}
static int cmd_unlock(int argc, const char **argv) {
    if (argc < 2) return cu_usage("unlock <file...>      (the user who locked it, or root)");
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int rc = sys_lock(argv[i], false);
        if (rc == SYS_EINVAL) { cu_error("unlock", "that file is not locked"); status = 1; continue; }
        if (rc != 0) { status = cu_fs_error("unlock", argv[i], rc, CU_FILE); continue; }
        screen_puts(argv[i]); screen_puts(" is unlocked\n");
    }
    return status;
}

/* ---------- quota (display only) ---------- */
#define QUOTA_UIDS 32
typedef struct { uint32_t files, dirs, bytes; } usage_t;
static void quota_cb(const char *path, const fs_stat_t *st, void *ctx) {
    (void)path;
    usage_t *u = ctx;
    if (st->uid >= QUOTA_UIDS) return;
    if (st->type == FS_DIR) u[st->uid].dirs++; else { u[st->uid].files++; u[st->uid].bytes += (uint32_t)st->size; }
}
static uint32_t quota_limit(const char *name) { return !strcmp(name, "root") ? 0 : !strcmp(name, "admin") ? 256u * 1024 : 64u * 1024; }
static void quota_line(const user_info_t *ui, const usage_t *u) {
    char sz[20], lim[20]; uint32_t limit = quota_limit(ui->name);
    cu_size_str(sz, u->bytes);
    if (limit) cu_size_str(lim, limit); else { const char *t = "none"; size_t k = 0; while (t[k]) { lim[k] = t[k]; k++; } lim[k] = 0; }
    cu_pad(ui->name, 10); cu_num_pad(u->files, 5); cu_num_pad(u->dirs, 6); screen_puts("  "); cu_pad(sz, 11); cu_pad(lim, 10);
    if (limit) { uint32_t pct = (u->bytes >> 6) * 100 / (limit >> 6); screen_put_dec(pct); screen_putc('%'); if (u->bytes > limit) screen_puts("  OVER THE LIMIT (not enforced)"); }
    screen_putc('\n');
}
static int cmd_quota(int argc, const char **argv) {
    bool all = argc == 2 && !strcmp(argv[1], "-a");
    if (argc > 2 || (argc == 2 && !all && argv[1][0] == '-')) return cu_usage("quota [user]   |   quota -a      (shows what each account stores; limits are display only: nothing is enforced)");
    usage_t *u = sys_memory_alloc(sizeof(usage_t) * QUOTA_UIDS);
    if (!u) return cu_error("quota", "out of memory");
    for (int i = 0; i < QUOTA_UIDS; i++) u[i].files = u[i].dirs = u[i].bytes = 0;
    uint32_t budget = 4000; walk_dir("/", 0, &budget, quota_cb, u);
    user_info_t ui; int status = 0;
    screen_puts("user      files  dirs  stored     limit     used\n");
    if (all) { for (uint32_t i = 0; sys_user_get(i, &ui) == 0; i++) if (ui.uid < QUOTA_UIDS) quota_line(&ui, &u[ui.uid]); }
    else {
        int rc;
        if (argc == 2) rc = sys_user_lookup(argv[1], -1, &ui);
        else { cred_info_t c; sys_getcred(&c); rc = sys_user_lookup(NULL, c.uid, &ui); }
        if (rc != 0) { cu_error("quota", "no such user"); status = 1; } else if (ui.uid < QUOTA_UIDS) quota_line(&ui, &u[ui.uid]);
    }
    if (budget == 0) screen_puts("(the tree is large: only the first 4000 entries were counted)\n");
    screen_puts("Limits are display only: MiniOS does not stop anyone from exceeding them.\n");
    sys_memory_free(u);
    return status;
}

static const struct { const char *name; cmd_handler_t fn; } bindings[] = {
    { "more", cmd_more }, { "ln", cmd_ln }, { "newer", cmd_newer }, { "diff", cmd_diff }, { "protect", cmd_protect }, { "unprotect", cmd_unprotect },
    { "lock", cmd_lock }, { "unlock", cmd_unlock }, { "quota", cmd_quota },
};

int fileextra_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
