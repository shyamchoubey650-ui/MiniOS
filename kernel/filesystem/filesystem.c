/* MiniFS core: node records, the directory tree, permissions, accounting and fsck.
 * Files and directories are heap-allocated nodes linked as parent / first_child /
 * next_sibling; the children of a directory are kept sorted by name, so listings and
 * walks are deterministic. File contents are heap buffers that grow on demand. */
#include "include/filesystem.h"
#include "include/memory.h"
#include "include/string.h"

typedef struct fs_node {
    uint32_t        ino;
    fs_type_t       type;
    uint16_t        mode, uid, gid;
    uint32_t        mtime, ctime;
    char            name[FS_NAME_MAX + 1];
    struct fs_node *parent, *first_child, *next_sibling;
    uint32_t        child_count;
    bool            protect;           /* see fs_set_protect */
    bool            locked;            /* see fs_set_lock */
    uint16_t        lock_uid;
    uint8_t        *data;              /* files */
    size_t          size, capacity;
} fs_node_t;

static fs_node_t *root, *cwd;
static uint32_t   next_ino, n_nodes, n_files, n_dirs;
static size_t     data_bytes, capacity_bytes;
static uint32_t (*clock_fn)(void);
static uint16_t   cur_uid = FS_UID_ROOT, cur_gid = FS_UID_ROOT;

static uint32_t now(void) { return clock_fn ? clock_fn() : 0; }
void fs_set_clock(uint32_t (*fn)(void)) { clock_fn = fn; }
void fs_set_user(uint16_t uid, uint16_t gid) { cur_uid = uid; cur_gid = gid; }
static uint16_t   cur_umask = 022;
void fs_set_umask(uint16_t mask) { cur_umask = mask & FS_MODE_MASK; }
uint16_t fs_get_umask(void) { return cur_umask; }
void fs_get_user(uint16_t *uid, uint16_t *gid) { if (uid) *uid = cur_uid; if (gid) *gid = cur_gid; }

/* ---------- permissions ---------- */
bool fs_mode_allows(uint16_t mode, uint16_t owner, uint16_t group, uint16_t uid, uint16_t gid, int want) {
    if (uid == FS_UID_ROOT) return true;                        /* root bypasses permission bits */
    int shift = (uid == owner) ? 6 : (gid == group) ? 3 : 0;
    return (((mode >> shift) & 7) & want) == want;
}
static int write_check(const fs_node_t *n);
static bool allowed(const fs_node_t *n, int want) { return fs_mode_allows(n->mode, n->uid, n->gid, cur_uid, cur_gid, want); }

static int lock_check(const fs_node_t *n) {                     /* advisory: root and the lock owner pass */
    if (n->locked && cur_uid != n->lock_uid && cur_uid != FS_UID_ROOT) return FS_ERR_LOCKED;
    return FS_OK;
}
static int write_check(const fs_node_t *n) {                    /* may this file's contents change? */
    int lk = lock_check(n);
    if (lk != FS_OK) return lk;
    if (n->protect) return FS_ERR_PROTECTED;
    return allowed(n, FS_W) ? FS_OK : FS_ERR_PERMISSION;
}

/* ---------- node management ---------- */
static fs_node_t *node_new(fs_type_t type, const char *name) {
    if (n_nodes >= FS_MAX_NODES) return NULL;
    fs_node_t *n = kcalloc(1, sizeof *n);
    if (!n) return NULL;
    n->ino = next_ino++;
    n->type = type;
    n->mode = (uint16_t)(((type == FS_DIR) ? 0777 : 0666) & ~cur_umask);   /* 0755 / 0644 with the default umask 022 */
    n->uid = cur_uid; n->gid = cur_gid;
    n->mtime = n->ctime = now();
    size_t i = 0; while (name[i] && i < FS_NAME_MAX) { n->name[i] = name[i]; i++; }
    n->name[i] = '\0';
    n_nodes++;
    if (type == FS_DIR) n_dirs++; else n_files++;
    return n;
}

static void node_free(fs_node_t *n) {
    if (n->data) { kfree(n->data); capacity_bytes -= n->capacity; }
    data_bytes -= n->size;
    n_nodes--;
    if (n->type == FS_DIR) n_dirs--; else n_files--;
    kfree(n);
}

static void link_child(fs_node_t *parent, fs_node_t *n) {       /* sorted insert */
    fs_node_t **pp = &parent->first_child;
    while (*pp && strcmp((*pp)->name, n->name) < 0) pp = &(*pp)->next_sibling;
    n->next_sibling = *pp; *pp = n;
    n->parent = parent; parent->child_count++;
    parent->mtime = now();
}

static void unlink_child(fs_node_t *n) {
    fs_node_t **pp = &n->parent->first_child;
    while (*pp && *pp != n) pp = &(*pp)->next_sibling;
    if (*pp) *pp = n->next_sibling;
    n->parent->child_count--;
    n->parent->mtime = now();
    n->parent = NULL; n->next_sibling = NULL;
}

static fs_node_t *find_child(const fs_node_t *dir, const char *name) {
    for (fs_node_t *c = dir->first_child; c; c = c->next_sibling) {
        int d = strcmp(c->name, name);
        if (d == 0) return c;
        if (d > 0) break;                                       /* sorted: it cannot be further on */
    }
    return NULL;
}

static void destroy_subtree(fs_node_t *n) {                     /* recursion depth <= FS_MAX_DEPTH */
    while (n->first_child) {
        fs_node_t *c = n->first_child;
        n->first_child = c->next_sibling;
        c->parent = NULL; n->child_count--;
        destroy_subtree(c);
    }
    node_free(n);
}

static bool is_ancestor_or_self(const fs_node_t *anc, const fs_node_t *n) {
    for (; n; n = n->parent) if (n == anc) return true;
    return false;
}

static int node_depth(const fs_node_t *n) { int d = 0; for (; n && n->parent; n = n->parent) d++; return d; }
static int subtree_height(const fs_node_t *n) {                 /* levels below n */
    int h = 0;
    for (const fs_node_t *c = n->first_child; c; c = c->next_sibling) { int k = 1 + subtree_height(c); if (k > h) h = k; }
    return h;
}

/* ---------- path resolution ---------- */
static void build_path(const fs_node_t *n, char *buf, size_t size) {
    const fs_node_t *chain[FS_MAX_DEPTH + 2]; size_t depth = 0;
    for (; n && n->parent && depth < FS_MAX_DEPTH + 1; n = n->parent) chain[depth++] = n;
    size_t len = 0;
    if (depth == 0) { if (size >= 2) { buf[0] = '/'; buf[1] = '\0'; } return; }
    while (depth--) {
        const char *name = chain[depth]->name;
        if (len + 1 + strlen(name) + 1 > size) break;
        buf[len++] = '/';
        for (size_t i = 0; name[i]; i++) buf[len++] = name[i];
    }
    buf[len] = '\0';
}

int fs_getcwd(char *buf, size_t size) {
    if (!buf || size < 2) return FS_ERR_INVALID;
    build_path(cwd, buf, size);
    return FS_OK;
}

int fs_resolve(const char *path, char *out, size_t outsz) {
    char cwdp[FS_PATH_MAX];
    if (!path || !out) return FS_ERR_INVALID;
    build_path(cwd, cwdp, sizeof cwdp);
    return fs_path_normalize(path, cwdp, out, outsz);
}

/* Finds the node for a canonical path, requiring search (x) permission on every directory passed through. */
static int lookup_canon(const char *canon, fs_node_t **out) {
    fs_node_t *n = root;
    const char *p = canon;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[FS_NAME_MAX + 1]; size_t k = 0;
        while (*p && *p != '/' && k < FS_NAME_MAX) comp[k++] = *p++;
        comp[k] = '\0';
        if (n->type != FS_DIR) return FS_ERR_NOT_DIR;
        if (!allowed(n, FS_X)) return FS_ERR_PERMISSION;
        n = find_child(n, comp);
        if (!n) return FS_ERR_NOT_FOUND;
    }
    *out = n;
    return FS_OK;
}

static int lookup(const char *path, fs_node_t **out) {
    char canon[FS_PATH_MAX];
    int rc = fs_resolve(path, canon, sizeof canon);
    if (rc != FS_OK) return rc;
    return lookup_canon(canon, out);
}

/* Finds the directory that holds (or would hold) `path` and the last component's name. */
static int lookup_parent(const char *path, fs_node_t **parent, char *name, char *canon_out) {
    char canon[FS_PATH_MAX], ppath[FS_PATH_MAX];
    int rc = fs_resolve(path, canon, sizeof canon);
    if (rc != FS_OK) return rc;
    rc = fs_path_split(canon, ppath, sizeof ppath, name, FS_NAME_MAX + 1);
    if (rc != FS_OK) return rc == FS_ERR_INVALID ? FS_ERR_EXISTS : rc;      /* "/" already exists */
    if (fs_path_depth(canon) > FS_MAX_DEPTH) return FS_ERR_PATH_TOO_LONG;
    if (canon_out) { size_t i = 0; while (canon[i]) { canon_out[i] = canon[i]; i++; } canon_out[i] = '\0'; }
    fs_node_t *p;
    rc = lookup_canon(ppath, &p);
    if (rc != FS_OK) return rc;
    if (p->type != FS_DIR) return FS_ERR_NOT_DIR;
    *parent = p;
    return FS_OK;
}

static void fill_stat(const fs_node_t *n, fs_stat_t *s) {
    s->ino = n->ino; s->type = n->type; s->mode = n->mode; s->uid = n->uid; s->gid = n->gid;
    s->protect = n->protect; s->locked = n->locked; s->lock_uid = n->lock_uid; s->size = n->size; s->children = n->child_count; s->mtime = n->mtime; s->ctime = n->ctime;
    size_t i = 0; while (n->name[i]) { s->name[i] = n->name[i]; i++; }
    if (!n->parent && n == root) { s->name[0] = '/'; i = 1; }
    s->name[i] = '\0';
}

int fs_stat(const char *path, fs_stat_t *out) {
    fs_node_t *n; int rc = lookup(path, &n);
    if (rc != FS_OK) return rc;
    if (out) fill_stat(n, out);
    return FS_OK;
}

int fs_chdir(const char *path) {
    fs_node_t *n; int rc = lookup(path, &n);
    if (rc != FS_OK) return rc;
    if (n->type != FS_DIR) return FS_ERR_NOT_DIR;
    if (!allowed(n, FS_X)) return FS_ERR_PERMISSION;
    cwd = n;
    return FS_OK;
}

/* ---------- creation ---------- */
static int make_node(const char *path, fs_type_t type, fs_node_t **created) {
    fs_node_t *parent; char name[FS_NAME_MAX + 1];
    int rc = lookup_parent(path, &parent, name, NULL);
    if (rc != FS_OK) return rc;
    if (!fs_name_valid(name)) return FS_ERR_INVALID;
    if (find_child(parent, name)) return FS_ERR_EXISTS;
    if (!allowed(parent, FS_W | FS_X)) return FS_ERR_PERMISSION;
    fs_node_t *n = node_new(type, name);
    if (!n) return FS_ERR_NO_SPACE;
    link_child(parent, n);
    if (created) *created = n;
    return FS_OK;
}

int fs_mkdir(const char *path)  { return make_node(path, FS_DIR, NULL); }
int fs_create(const char *path) { return make_node(path, FS_FILE, NULL); }

int fs_mkdir_parents(const char *path) {
    char canon[FS_PATH_MAX];
    int rc = fs_resolve(path, canon, sizeof canon);
    if (rc != FS_OK) return rc;
    if (fs_path_depth(canon) > FS_MAX_DEPTH) return FS_ERR_PATH_TOO_LONG;
    char partial[FS_PATH_MAX]; size_t len = 0;
    for (const char *p = canon; *p;) {
        partial[len++] = *p++;                                  /* the leading '/' */
        while (*p && *p != '/') partial[len++] = *p++;
        partial[len] = '\0';
        if (len == 1) continue;                                 /* only "/" so far */
        fs_node_t *n;
        rc = lookup_canon(partial, &n);
        if (rc == FS_OK) { if (n->type != FS_DIR) return FS_ERR_NOT_DIR; continue; }
        if (rc != FS_ERR_NOT_FOUND) return rc;
        rc = make_node(partial, FS_DIR, NULL);
        if (rc != FS_OK) return rc;
    }
    return FS_OK;
}

int fs_touch(const char *path) {
    fs_node_t *n; int rc = lookup(path, &n);
    if (rc == FS_ERR_NOT_FOUND) return fs_create(path);
    if (rc != FS_OK) return rc;
    if (!allowed(n, FS_W)) return FS_ERR_PERMISSION;
    n->mtime = now();
    return FS_OK;
}

/* ---------- removal ---------- */
static int remove_checks(fs_node_t *n, fs_node_t *parent) {
    if (n == root) return FS_ERR_BUSY;
    { int lk = lock_check(n); if (lk != FS_OK) return lk; }
    if (n->protect) return FS_ERR_PROTECTED;
    if (!allowed(parent, FS_W | FS_X)) return FS_ERR_PERMISSION;
    if (is_ancestor_or_self(n, cwd)) return FS_ERR_BUSY;
    return FS_OK;
}

int fs_remove(const char *path) {
    fs_node_t *n; int rc = lookup(path, &n);
    if (rc != FS_OK) return rc;
    if (n->type == FS_DIR) return FS_ERR_IS_DIR;
    rc = remove_checks(n, n->parent);
    if (rc != FS_OK) return rc;
    unlink_child(n); node_free(n);
    return FS_OK;
}

int fs_rmdir(const char *path) {
    fs_node_t *n; int rc = lookup(path, &n);
    if (rc != FS_OK) return rc;
    if (n->type != FS_DIR) return FS_ERR_NOT_DIR;
    if (n == root) return FS_ERR_BUSY;
    if (n->protect) return FS_ERR_PROTECTED;
    if (is_ancestor_or_self(n, cwd)) return FS_ERR_BUSY;
    if (n->child_count) return FS_ERR_NOT_EMPTY;
    if (!allowed(n->parent, FS_W | FS_X)) return FS_ERR_PERMISSION;
    unlink_child(n); node_free(n);
    return FS_OK;
}

static bool tree_removable(const fs_node_t *n) {                /* directories must be writable+searchable to be emptied */
    if (n->type == FS_DIR) {
        if (n->child_count && !allowed(n, FS_W | FS_X)) return false;
        for (const fs_node_t *c = n->first_child; c; c = c->next_sibling) if (!tree_removable(c)) return false;
    }
    return true;
}

static bool tree_has_protected(const fs_node_t *n) {
    if (n->protect) return true;
    for (const fs_node_t *c = n->first_child; c; c = c->next_sibling) if (tree_has_protected(c)) return true;
    return false;
}

static uint32_t count_nodes(const fs_node_t *n) {
    uint32_t k = 1;
    for (const fs_node_t *c = n->first_child; c; c = c->next_sibling) k += count_nodes(c);
    return k;
}

int fs_remove_tree(const char *path, uint32_t *removed) {
    fs_node_t *n; int rc = lookup(path, &n);
    if (rc != FS_OK) return rc;
    rc = remove_checks(n, n->parent ? n->parent : n);
    if (rc != FS_OK) return rc;
    if (tree_has_protected(n)) return FS_ERR_PROTECTED;         /* all or nothing */
    if (!tree_removable(n)) return FS_ERR_PERMISSION;
    uint32_t k = count_nodes(n);
    unlink_child(n);
    destroy_subtree(n);
    if (removed) *removed = k;
    return FS_OK;
}

/* ---------- file contents ---------- */
static int set_capacity(fs_node_t *n, size_t cap) {             /* exact reallocation; old data is preserved up to n->size */
    uint8_t *nd = NULL;
    if (cap) {
        nd = kmalloc(cap);
        if (!nd) return FS_ERR_NO_SPACE;
        size_t keep = n->size < cap ? n->size : cap;
        if (keep) memcpy(nd, n->data, keep);
    }
    if (n->data) { kfree(n->data); capacity_bytes -= n->capacity; }
    n->data = nd; n->capacity = cap; capacity_bytes += cap;
    return FS_OK;
}

static int file_for_write(const char *path, fs_node_t **out) {
    fs_node_t *n; int rc = lookup(path, &n);
    if (rc == FS_ERR_NOT_FOUND) {
        rc = make_node(path, FS_FILE, &n);
        if (rc != FS_OK) return rc;
    } else if (rc != FS_OK) return rc;
    else {
        if (n->type == FS_DIR) return FS_ERR_IS_DIR;
        rc = write_check(n);
        if (rc != FS_OK) return rc;
    }
    *out = n;
    return FS_OK;
}

int fs_can_write(const char *path) {
    fs_node_t *n; int rc = lookup(path, &n);
    if (rc == FS_OK) {
        if (n->type == FS_DIR) return FS_ERR_IS_DIR;
        return write_check(n);
    }
    if (rc != FS_ERR_NOT_FOUND) return rc;
    fs_node_t *parent; char name[FS_NAME_MAX + 1];
    rc = lookup_parent(path, &parent, name, NULL);
    if (rc != FS_OK) return rc;
    if (!fs_name_valid(name)) return FS_ERR_INVALID;
    return allowed(parent, FS_W | FS_X) ? FS_OK : FS_ERR_PERMISSION;
}

int fs_write(const char *path, const void *data, size_t len, fs_write_mode_t mode) {
    if (len && !data) return FS_ERR_INVALID;
    fs_node_t *n; fs_node_t *existing = NULL;
    bool existed = lookup(path, &existing) == FS_OK;
    int rc = file_for_write(path, &n);
    if (rc != FS_OK) return rc;
    size_t base = (mode == FS_WRITE_APPEND) ? n->size : 0;
    if (len > FS_MAX_FILE_SIZE || base > FS_MAX_FILE_SIZE - len) rc = FS_ERR_TOO_BIG;
    else if (base + len > n->capacity || mode == FS_WRITE_TRUNCATE) {
        size_t need = base + len, cap = need;
        if (mode == FS_WRITE_APPEND && n->capacity && cap < n->capacity * 2) cap = n->capacity * 2 > FS_MAX_FILE_SIZE ? FS_MAX_FILE_SIZE : n->capacity * 2;
        if (mode == FS_WRITE_TRUNCATE && need <= n->capacity && need != 0) cap = n->capacity;   /* reuse the buffer */
        if (cap != n->capacity) rc = set_capacity(n, cap);
        else rc = FS_OK;
    }
    if (rc != FS_OK) {                                          /* nothing changed; drop a file we just created */
        if (!existed && n->size == 0 && !n->data) { unlink_child(n); node_free(n); }
        return rc;
    }
    if (mode == FS_WRITE_TRUNCATE) { data_bytes -= n->size; n->size = 0; }
    if (len) memcpy(n->data + base, data, len);
    n->size = base + len; data_bytes += len;
    n->mtime = now();
    return FS_OK;
}

int fs_write_at(const char *path, size_t offset, const void *data, size_t len) {
    if (len && !data) return FS_ERR_INVALID;
    fs_node_t *n; int rc = lookup(path, &n);
    if (rc != FS_OK) return rc;
    if (n->type == FS_DIR) return FS_ERR_IS_DIR;
    rc = write_check(n);
    if (rc != FS_OK) return rc;
    if (len == 0) return FS_OK;
    if (offset > FS_MAX_FILE_SIZE || len > FS_MAX_FILE_SIZE - offset) return FS_ERR_TOO_BIG;   /* overflow-safe */
    size_t end = offset + len;
    if (end > n->capacity) {
        rc = set_capacity(n, end);                              /* old contents are preserved; on failure nothing changed */
        if (rc != FS_OK) return rc;
    }
    if (offset > n->size) memset(n->data + n->size, 0, offset - n->size);   /* sparse gap reads as zeros */
    memcpy(n->data + offset, data, len);
    if (end > n->size) { data_bytes += end - n->size; n->size = end; }
    n->mtime = now();
    return FS_OK;
}

int fs_read(const char *path, void *buf, size_t max, size_t offset, size_t *out_len) {
    fs_node_t *n; int rc = lookup(path, &n);
    if (rc != FS_OK) return rc;
    if (n->type == FS_DIR) return FS_ERR_IS_DIR;
    if (!allowed(n, FS_R)) return FS_ERR_PERMISSION;
    if (max && !buf) return FS_ERR_INVALID;
    size_t got = 0;
    if (offset < n->size) { got = n->size - offset; if (got > max) got = max; }
    if (got) memcpy(buf, n->data + offset, got);
    if (out_len) *out_len = got;
    return FS_OK;
}

int fs_truncate(const char *path, size_t new_size) {
    if (new_size > FS_MAX_FILE_SIZE) return FS_ERR_TOO_BIG;
    fs_node_t *n; int rc = lookup(path, &n);
    if (rc != FS_OK) return rc;
    if (n->type == FS_DIR) return FS_ERR_IS_DIR;
    rc = write_check(n);
    if (rc != FS_OK) return rc;
    if (new_size > n->size) {
        if (new_size > n->capacity) { rc = set_capacity(n, new_size); if (rc != FS_OK) return rc; }
        memset(n->data + n->size, 0, new_size - n->size);
    } else if (new_size == 0 && n->data) set_capacity(n, 0);
    data_bytes = data_bytes - n->size + new_size;
    n->size = new_size; n->mtime = now();
    return FS_OK;
}

int fs_swap_data(const char *a, const char *b) {
    fs_node_t *x, *y; int rc = lookup(a, &x);
    if (rc != FS_OK) return rc;
    rc = lookup(b, &y);
    if (rc != FS_OK) return rc;
    if (x->type == FS_DIR || y->type == FS_DIR) return FS_ERR_IS_DIR;
    if ((rc = write_check(x)) != FS_OK || (rc = write_check(y)) != FS_OK) return rc;
    if (x == y) return FS_OK;
    uint8_t *d = x->data; x->data = y->data; y->data = d;
    size_t t = x->size; x->size = y->size; y->size = t;
    t = x->capacity; x->capacity = y->capacity; y->capacity = t;
    x->mtime = y->mtime = now();
    return FS_OK;
}

/* ---------- rename / copy ---------- */
int fs_rename(const char *from, const char *to) {
    fs_node_t *n; int rc = lookup(from, &n);
    if (rc != FS_OK) return rc;
    if (n == root) return FS_ERR_BUSY;
    { int lk = lock_check(n); if (lk != FS_OK) return lk; }
    if (n->protect) return FS_ERR_PROTECTED;
    fs_node_t *dest_parent; char name[FS_NAME_MAX + 1];
    rc = lookup_parent(to, &dest_parent, name, NULL);
    if (rc != FS_OK) return rc;
    if (!fs_name_valid(name)) return FS_ERR_INVALID;
    if (find_child(dest_parent, name)) return FS_ERR_EXISTS;
    if (n->type == FS_DIR && is_ancestor_or_self(n, dest_parent)) return FS_ERR_INTO_SELF;
    if (node_depth(dest_parent) + 1 + (n->type == FS_DIR ? subtree_height(n) : 0) > FS_MAX_DEPTH) return FS_ERR_PATH_TOO_LONG;
    if (!allowed(n->parent, FS_W | FS_X) || !allowed(dest_parent, FS_W | FS_X)) return FS_ERR_PERMISSION;
    unlink_child(n);
    size_t i = 0; while (name[i]) { n->name[i] = name[i]; i++; }
    n->name[i] = '\0';
    link_child(dest_parent, n);
    n->mtime = now();
    return FS_OK;
}

static int copy_contents(const fs_node_t *src, fs_node_t *dst) {
    if (src->size > dst->capacity) { int rc = set_capacity(dst, src->size); if (rc != FS_OK) return rc; }
    if (src->size) memcpy(dst->data, src->data, src->size);
    data_bytes = data_bytes - dst->size + src->size;
    dst->size = src->size; dst->mtime = now();
    return FS_OK;
}

int fs_copy(const char *from, const char *to) {
    fs_node_t *src; int rc = lookup(from, &src);
    if (rc != FS_OK) return rc;
    if (src->type == FS_DIR) return FS_ERR_IS_DIR;
    if (!allowed(src, FS_R)) return FS_ERR_PERMISSION;
    fs_node_t *dst; bool created = false;
    rc = lookup(to, &dst);
    if (rc == FS_OK) {
        if (dst == src) return FS_ERR_EXISTS;                   /* same file */
        if (dst->type == FS_DIR) return FS_ERR_IS_DIR;
        rc = write_check(dst);
        if (rc != FS_OK) return rc;
    } else if (rc == FS_ERR_NOT_FOUND) {
        rc = make_node(to, FS_FILE, &dst);
        if (rc != FS_OK) return rc;
        dst->mode = src->mode; created = true;
    } else return rc;
    rc = copy_contents(src, dst);
    if (rc != FS_OK && created) { unlink_child(dst); node_free(dst); }
    return rc;
}

static int copy_node_tree(const fs_node_t *src, fs_node_t *dst_dir) {      /* copies src's children into dst_dir */
    for (const fs_node_t *c = src->first_child; c; c = c->next_sibling) {
        if (n_nodes >= FS_MAX_NODES) return FS_ERR_NO_SPACE;
        fs_node_t *k = node_new(c->type, c->name);
        if (!k) return FS_ERR_NO_SPACE;
        k->mode = c->mode;
        link_child(dst_dir, k);
        int rc = (c->type == FS_DIR) ? copy_node_tree(c, k) : copy_contents(c, k);
        if (rc != FS_OK) return rc;
    }
    return FS_OK;
}

static bool tree_readable(const fs_node_t *n) {
    if (n->type == FS_FILE) return allowed(n, FS_R);
    if (!allowed(n, FS_R | FS_X)) return false;
    for (const fs_node_t *c = n->first_child; c; c = c->next_sibling) if (!tree_readable(c)) return false;
    return true;
}

int fs_copy_tree(const char *from, const char *to) {
    fs_node_t *src; int rc = lookup(from, &src);
    if (rc != FS_OK) return rc;
    if (src->type == FS_FILE) {
        fs_node_t *probe;
        if (lookup(to, &probe) == FS_OK) return FS_ERR_EXISTS;
        return fs_copy(from, to);
    }
    fs_node_t *dest_parent; char name[FS_NAME_MAX + 1];
    rc = lookup_parent(to, &dest_parent, name, NULL);
    if (rc != FS_OK) return rc;
    if (!fs_name_valid(name)) return FS_ERR_INVALID;
    if (find_child(dest_parent, name)) return FS_ERR_EXISTS;
    if (is_ancestor_or_self(src, dest_parent)) return FS_ERR_INTO_SELF;
    if (node_depth(dest_parent) + 1 + subtree_height(src) > FS_MAX_DEPTH) return FS_ERR_PATH_TOO_LONG;
    if (!tree_readable(src)) return FS_ERR_PERMISSION;
    if (!allowed(dest_parent, FS_W | FS_X)) return FS_ERR_PERMISSION;
    if (n_nodes + count_nodes(src) > FS_MAX_NODES) return FS_ERR_NO_SPACE;

    fs_node_t *top = node_new(FS_DIR, name);
    if (!top) return FS_ERR_NO_SPACE;
    top->mode = src->mode;
    link_child(dest_parent, top);
    rc = copy_node_tree(src, top);
    if (rc != FS_OK) { unlink_child(top); destroy_subtree(top); }   /* roll back the partial copy */
    return rc;
}

/* ---------- attributes ---------- */
int fs_chmod(const char *path, uint16_t mode) {
    if (mode & ~FS_MODE_MASK) return FS_ERR_INVALID;
    fs_node_t *n; int rc = lookup(path, &n);
    if (rc != FS_OK) return rc;
    if (cur_uid != FS_UID_ROOT && cur_uid != n->uid) return FS_ERR_PERMISSION;
    n->mode = mode; n->ctime = now();
    return FS_OK;
}

int fs_set_lock(const char *path, bool on) {
    fs_node_t *n; int rc = lookup(path, &n);
    if (rc != FS_OK) return rc;
    if (n->type == FS_DIR) return FS_ERR_IS_DIR;                  /* locks guard file contents */
    if (on) {
        if (n->locked && n->lock_uid != cur_uid && cur_uid != FS_UID_ROOT) return FS_ERR_LOCKED;
        if (!allowed(n, FS_W)) return FS_ERR_PERMISSION;          /* you can only lock what you could write anyway */
        n->locked = true; n->lock_uid = cur_uid;
    } else {
        if (!n->locked) return FS_ERR_INVALID;
        if (cur_uid != n->lock_uid && cur_uid != FS_UID_ROOT) return FS_ERR_LOCKED;
        n->locked = false; n->lock_uid = 0;
    }
    return FS_OK;
}

int fs_set_protect(const char *path, bool on) {
    fs_node_t *n; int rc = lookup(path, &n);
    if (rc != FS_OK) return rc;
    if (n == root) return FS_ERR_BUSY;
    if (cur_uid != FS_UID_ROOT && cur_uid != n->uid) return FS_ERR_PERMISSION;
    n->protect = on; n->ctime = now();
    return FS_OK;
}

int fs_chown(const char *path, uint16_t uid, uint16_t gid) {
    fs_node_t *n; int rc = lookup(path, &n);
    if (rc != FS_OK) return rc;
    if (cur_uid != FS_UID_ROOT) return FS_ERR_PERMISSION;
    n->uid = uid; n->gid = gid; n->ctime = now();
    return FS_OK;
}

/* ---------- listing and walking ---------- */
int fs_readdir(const char *path, fs_dirent_fn fn, void *ctx) {
    fs_node_t *n; int rc = lookup(path, &n);
    if (rc != FS_OK) return rc;
    if (n->type != FS_DIR) return FS_ERR_NOT_DIR;
    if (!allowed(n, FS_R)) return FS_ERR_PERMISSION;
    for (const fs_node_t *c = n->first_child; c; c = c->next_sibling) {
        fs_dirent_t e;
        size_t i = 0; while (c->name[i]) { e.name[i] = c->name[i]; i++; }
        e.name[i] = '\0'; e.type = c->type; e.size = c->size; e.mode = c->mode; e.ino = c->ino;
        if (fn && !fn(&e, ctx)) break;
    }
    return FS_OK;
}

static bool walk_node(const fs_node_t *n, char *path, size_t len, int depth, fs_walk_fn fn, void *ctx) {
    fs_stat_t st; fill_stat(n, &st);
    if (!fn(path, &st, depth, ctx)) return false;
    if (n->type != FS_DIR || !allowed(n, FS_R | FS_X)) return true;      /* unreadable directories are skipped, not errors */
    for (const fs_node_t *c = n->first_child; c; c = c->next_sibling) {
        size_t nl = strlen(c->name);
        size_t base = (len == 1) ? 0 : len;
        if (base + 1 + nl + 1 > FS_PATH_MAX) continue;
        path[base] = '/';
        for (size_t i = 0; i < nl; i++) path[base + 1 + i] = c->name[i];
        path[base + 1 + nl] = '\0';
        bool go = walk_node(c, path, base + 1 + nl, depth + 1, fn, ctx);
        path[len] = '\0';
        if (len == 0) path[0] = '\0';
        if (!go) return false;
    }
    return true;
}

int fs_walk(const char *start, fs_walk_fn fn, void *ctx) {
    char canon[FS_PATH_MAX];
    if (!fn) return FS_ERR_INVALID;
    int rc = fs_resolve(start, canon, sizeof canon);
    if (rc != FS_OK) return rc;
    fs_node_t *n;
    rc = lookup_canon(canon, &n);
    if (rc != FS_OK) return rc;
    size_t len = strlen(canon);
    walk_node(n, canon, len, 0, fn, ctx);
    return FS_OK;
}

typedef struct { size_t bytes; uint32_t files, dirs; } size_acc_t;
static void size_rec(const fs_node_t *n, size_acc_t *a) {
    if (n->type == FS_FILE) { a->bytes += n->size; a->files++; return; }
    a->dirs++;
    for (const fs_node_t *c = n->first_child; c; c = c->next_sibling) size_rec(c, a);
}

int fs_tree_size(const char *path, size_t *bytes, uint32_t *files, uint32_t *dirs) {
    fs_node_t *n; int rc = lookup(path, &n);
    if (rc != FS_OK) return rc;
    size_acc_t a = { 0, 0, 0 };
    size_rec(n, &a);
    if (bytes) *bytes = a.bytes;
    if (files) *files = a.files;
    if (dirs) *dirs = a.dirs;
    return FS_OK;
}

/* ---------- accounting ---------- */
void fs_usage(fs_usage_t *u) {
    if (!u) return;
    u->nodes = n_nodes; u->files = n_files; u->dirs = n_dirs; u->max_nodes = FS_MAX_NODES;
    u->data_bytes = data_bytes; u->capacity_bytes = capacity_bytes;
    u->node_bytes = (size_t)n_nodes * sizeof(fs_node_t);
}

/* ---------- consistency check (fsck) ---------- */
typedef struct { fs_check_report_t *r; uint32_t seen, files, dirs; size_t bytes, cap; } fsck_ctx_t;

static void problem(fsck_ctx_t *c, const char *what, const fs_node_t *n) {
    if (c->r->problems < 4) {
        char *dst = c->r->first[c->r->problems]; size_t i = 0;
        for (const char *s = what; *s && i < 40; s++) dst[i++] = *s;
        dst[i++] = ' '; dst[i++] = '(';
        for (const char *s = n ? n->name : "?"; *s && i < 66; s++) dst[i++] = *s;
        dst[i++] = ')'; dst[i] = '\0';
    }
    c->r->problems++;
}

static void fsck_node(const fs_node_t *n, fsck_ctx_t *c, int depth) {
    c->seen++;
    if (depth > FS_MAX_DEPTH) { problem(c, "nesting too deep", n); return; }
    if (n->type == FS_FILE) {
        c->files++; c->bytes += n->size; c->cap += n->capacity;
        if (n->first_child || n->child_count) problem(c, "file has children", n);
        if (n->size > n->capacity) problem(c, "size exceeds capacity", n);
        if (n->capacity && !n->data) problem(c, "capacity without buffer", n);
        if (!n->capacity && n->data) problem(c, "buffer without capacity", n);
        if (n->size > FS_MAX_FILE_SIZE) problem(c, "file over size limit", n);
        return;
    }
    if (n->type != FS_DIR) { problem(c, "bad node type", n); return; }
    c->dirs++;
    uint32_t kids = 0;
    const fs_node_t *prev = NULL;
    for (const fs_node_t *k = n->first_child; k; prev = k, k = k->next_sibling) {
        kids++;
        if (kids > FS_MAX_NODES) { problem(c, "directory list loops", n); return; }
        if (k->parent != n) problem(c, "wrong parent pointer", k);
        if (!fs_name_valid(k->name)) problem(c, "invalid name", k);
        if (prev && strcmp(prev->name, k->name) >= 0) problem(c, "children unsorted or duplicate", k);
        fsck_node(k, c, depth + 1);
    }
    if (kids != n->child_count) problem(c, "child count mismatch", n);
}

int fs_check(fs_check_report_t *r) {
    fs_check_report_t local; if (!r) r = &local;
    memset(r, 0, sizeof *r);
    fsck_ctx_t c = { r, 0, 0, 0, 0, 0 };
    if (!root) { r->problems = 1; return 1; }
    if (root->parent) problem(&c, "root has a parent", root);
    if (root->type != FS_DIR) problem(&c, "root is not a directory", root);
    fsck_node(root, &c, 0);
    if (!is_ancestor_or_self(root, cwd)) problem(&c, "working directory detached", cwd);
    r->nodes_seen = c.seen;
    if (c.seen != n_nodes) problem(&c, "leak: node count > reachable nodes", root);
    if (c.files != n_files || c.dirs != n_dirs) problem(&c, "file/dir counters wrong", root);
    if (c.bytes != data_bytes) problem(&c, "data byte counter wrong", root);
    if (c.cap != capacity_bytes) problem(&c, "capacity counter wrong", root);
    return (int)r->problems;
}

/* ---------- lifecycle ---------- */
static fs_node_t *mk(fs_node_t *parent, const char *name, fs_type_t type, uint16_t mode, uint16_t uid, uint16_t gid) {
    fs_node_t *n = node_new(type, name);
    if (!n) return NULL;
    n->mode = mode; n->uid = uid; n->gid = gid;
    link_child(parent, n);
    return n;
}

static void put_text(fs_node_t *f, const char *text) {
    size_t len = strlen(text);
    if (set_capacity(f, len) != FS_OK) return;
    memcpy(f->data, text, len);
    f->size = len; data_bytes += len;
}

int fs_init(void) {
    uint16_t su = cur_uid, sg = cur_gid, sm = cur_umask;
    cur_umask = 022;                                            /* the standard layout does not depend on the caller's umask */
    if (root) { cwd = NULL; destroy_subtree(root); root = NULL; }
    n_nodes = n_files = n_dirs = 0; data_bytes = capacity_bytes = 0; next_ino = 1;
    cur_uid = cur_gid = FS_UID_ROOT;                            /* the layout is created by root */

    root = node_new(FS_DIR, "");
    if (!root) { cur_uid = su; cur_gid = sg; cur_umask = sm; return FS_ERR_NO_SPACE; }
    cwd = root;
    fs_node_t *home, *var;
    bool ok = mk(root, "bin", FS_DIR, 0755, 0, 0) && mk(root, "etc", FS_DIR, 0755, 0, 0) &&
              (home = mk(root, "home", FS_DIR, 0755, 0, 0)) &&
              mk(home, "admin", FS_DIR, 0750, FS_UID_ADMIN, FS_UID_ADMIN) &&
              mk(home, "student", FS_DIR, 0750, FS_UID_STUDENT, FS_UID_STUDENT) &&
              mk(root, "tmp", FS_DIR, 0777, 0, 0) && mk(root, "usr", FS_DIR, 0755, 0, 0) &&
              (var = mk(root, "var", FS_DIR, 0755, 0, 0)) && mk(var, "log", FS_DIR, 0755, 0, 0);
    if (ok) {
        fs_node_t *etc = find_child(root, "etc"), *f;
        if ((f = mk(etc, "motd", FS_FILE, 0644, 0, 0)))     put_text(f, "Welcome to MiniOS - an educational operating system.\n");
        if ((f = mk(etc, "hostname", FS_FILE, 0644, 0, 0))) put_text(f, "minios\n");
    }
    cur_uid = su; cur_gid = sg; cur_umask = sm;
    return ok ? FS_OK : FS_ERR_NO_SPACE;
}

const char *fs_strerror(int c) {
    switch (c) {
        case FS_OK:               return "ok";
        case FS_ERR_NOT_FOUND:    return "no such file or directory";
        case FS_ERR_EXISTS:       return "already exists";
        case FS_ERR_NOT_DIR:      return "not a directory";
        case FS_ERR_IS_DIR:       return "is a directory";
        case FS_ERR_NOT_EMPTY:    return "directory not empty";
        case FS_ERR_INVALID:      return "invalid name or argument";
        case FS_ERR_NAME_TOO_LONG: return "name too long (max 31 characters)";
        case FS_ERR_PATH_TOO_LONG: return "path too long or nested too deeply";
        case FS_ERR_NO_SPACE:     return "file system full (node limit or out of memory)";
        case FS_ERR_TOO_BIG:      return "file too large (max 256 KiB)";
        case FS_ERR_PERMISSION:   return "permission denied";
        case FS_ERR_PROTECTED:    return "protected (use unprotect first)";
        case FS_ERR_BUSY:         return "busy (root or contains the working directory)";
        case FS_ERR_INTO_SELF:    return "cannot move or copy a directory into itself";
        default:                  return "unknown file system error";
    }
}

void fs_mode_string(fs_type_t type, uint16_t mode, char out[11]) {
    static const char bits[] = "rwxrwxrwx";
    out[0] = type == FS_DIR ? 'd' : '-';
    for (int i = 0; i < 9; i++) out[1 + i] = (mode & (1u << (8 - i))) ? bits[i] : '-';
    out[10] = '\0';
}
