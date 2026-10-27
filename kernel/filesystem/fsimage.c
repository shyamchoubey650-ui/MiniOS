/* MiniFS image on the RAM-disk (savefs / loadfs).
 *
 * Layout, starting at sector 0 (all integers little-endian):
 *   header (24 bytes): "MFSI", version u32, node count u32, payload bytes u32, FNV-1a checksum of the payload u32
 *   payload, one record per node in walk order (parents before children), "/" excluded:
 *       path length u16, path bytes, type u8, protect u8, mode u16, uid u16, gid u16, data size u32, data bytes
 * The header is 24 bytes: the five fields above plus the tick count when it was written.
 * loadfs checks the magic, the checksum and every length BEFORE it touches the live file system, so a damaged or
 * foreign image can never leave MiniFS half-restored. */
#include "include/ramdisk.h"
#include "include/filesystem.h"
#include "include/syscall.h"
#include "include/memory.h"
#include "include/timer.h"
#include "include/string.h"

#define IMG_MAGIC   0x4953464Du          /* "MFSI" */
#define IMG_VERSION 1u
#define HDR_BYTES   24u                  /* magic, version, nodes, payload bytes, checksum, tick */

static uint32_t fnv1a(const uint8_t *p, size_t n) { uint32_t h = 2166136261u; while (n--) { h ^= *p++; h *= 16777619u; } return h; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* ---------- save ---------- */
typedef struct { size_t bytes; uint32_t nodes; uint8_t *out; size_t pos; int err; } save_ctx_t;

static bool save_cb(const char *path, const fs_stat_t *st, int depth, void *vc) {
    save_ctx_t *c = vc;
    if (depth == 0) return true;                                  /* "/" is recreated by fs_init */
    size_t pl = strlen(path), dl = st->type == FS_FILE ? st->size : 0;
    size_t rec = 2 + pl + 1 + 1 + 2 + 2 + 2 + 4 + dl;
    if (!c->out) { c->bytes += rec; c->nodes++; return true; }    /* sizing pass */
    uint8_t *p = c->out + c->pos;
    put16(p, (uint16_t)pl); memcpy(p + 2, path, pl); p += 2 + pl;
    *p++ = (uint8_t)st->type; *p++ = st->protect ? 1 : 0;
    put16(p, st->mode); put16(p + 2, st->uid); put16(p + 4, st->gid); p += 6;
    put32(p, (uint32_t)dl); p += 4;
    if (dl) {
        size_t got = 0;
        int rc = fs_read(path, p, dl, 0, &got);
        if (rc != FS_OK || got != dl) { c->err = rc ? rc : SYS_EINVAL; return false; }
    }
    c->pos += rec; c->nodes++;
    return true;
}

int fsimage_save(uint32_t *nodes, uint32_t *bytes) {
    if (!ramdisk_ready()) return SYS_EBLOCK;
    save_ctx_t c = { 0, 0, NULL, 0, 0 };
    int rc = fs_walk("/", save_cb, &c);
    if (rc != FS_OK) return rc;
    size_t total = HDR_BYTES + c.bytes;
    if (total > (size_t)RAMDISK_SECTORS * RAMDISK_SECTOR) return SYS_ENOSPC;
    size_t sectors = (total + RAMDISK_SECTOR - 1) / RAMDISK_SECTOR;
    uint8_t *img = kcalloc(sectors, RAMDISK_SECTOR);
    if (!img) return SYS_ENOMEM;
    save_ctx_t w = { 0, 0, img + HDR_BYTES, 0, 0 };
    rc = fs_walk("/", save_cb, &w);
    if (rc == FS_OK && w.err) rc = w.err;
    if (rc == FS_OK && (w.nodes != c.nodes || w.pos != c.bytes)) rc = SYS_ESTALE;   /* the tree changed while we copied it */
    if (rc != FS_OK) { kfree(img); return rc; }
    put32(img, IMG_MAGIC); put32(img + 4, IMG_VERSION); put32(img + 8, w.nodes); put32(img + 12, (uint32_t)w.pos);
    put32(img + 16, fnv1a(img + HDR_BYTES, w.pos)); put32(img + 20, timer_ticks());
    for (size_t s = 0; s < sectors && rc == 0; s++) rc = ramdisk_write((uint32_t)s, img + s * RAMDISK_SECTOR);
    kfree(img);
    if (rc == 0) { if (nodes) *nodes = w.nodes; if (bytes) *bytes = (uint32_t)total; }
    return rc;
}

/* ---------- describe ---------- */
void fsimage_describe(ramdisk_info_t *o) {
    uint8_t h[RAMDISK_SECTOR];
    o->image_valid = false;
    if (!ramdisk_ready()) return;
    if (ramdisk_peek(0, h) != 0) return;                          /* inspecting the header is not user I/O */
    if (get32(h) != IMG_MAGIC || get32(h + 4) != IMG_VERSION) return;
    o->image_valid = true; o->image_nodes = get32(h + 8); o->image_bytes = HDR_BYTES + get32(h + 12); o->image_tick = get32(h + 20);
}

/* ---------- load ---------- */
static bool name_ok(const uint8_t *p, size_t n) {                  /* absolute, printable, no "//" "." ".." tricks */
    if (n < 2 || n >= FS_PATH_MAX || p[0] != '/') return false;
    for (size_t i = 0; i < n; i++) if (p[i] < 33 || p[i] > 126) return false;
    return true;
}

/* Walks the records, calling apply(...) for each if non-NULL; returns false on any malformed record. */
typedef struct { const char *path; int type; bool protect; uint16_t mode, uid, gid; const uint8_t *data; uint32_t size; } rec_t;

static bool parse(const uint8_t *p, size_t len, uint32_t want_nodes, bool (*apply)(const rec_t *, void *), void *ctx) {
    size_t pos = 0; uint32_t seen = 0;
    while (pos < len) {
        if (len - pos < 2) return false;
        size_t pl = get16(p + pos); pos += 2;
        if (pl == 0 || len - pos < pl + 1 + 1 + 6 + 4) return false;
        char path[FS_PATH_MAX];
        if (!name_ok(p + pos, pl)) return false;
        memcpy(path, p + pos, pl); path[pl] = '\0'; pos += pl;
        rec_t r; r.path = path; r.type = p[pos]; r.protect = p[pos + 1] != 0; pos += 2;
        r.mode = get16(p + pos); r.uid = get16(p + pos + 2); r.gid = get16(p + pos + 4); pos += 6;
        r.size = get32(p + pos); pos += 4;
        if ((r.type != FS_FILE && r.type != FS_DIR) || (r.mode & ~FS_MODE_MASK)) return false;
        if (r.type == FS_DIR && r.size != 0) return false;
        if (r.size > FS_MAX_FILE_SIZE || len - pos < r.size) return false;
        r.data = p + pos; pos += r.size;
        if (apply && !apply(&r, ctx)) return false;
        seen++;
    }
    return seen == want_nodes;
}

static bool apply_rec(const rec_t *r, void *unused) {
    (void)unused;
    int rc;
    if (r->type == FS_DIR) { fs_stat_t st; rc = fs_stat(r->path, &st); if (rc == FS_ERR_NOT_FOUND) rc = fs_mkdir(r->path); else if (rc == FS_OK && st.type != FS_DIR) rc = FS_ERR_NOT_DIR; }
    else rc = fs_write(r->path, r->data, r->size, FS_WRITE_TRUNCATE);
    if (rc != FS_OK) return false;
    if (fs_chmod(r->path, r->mode) != FS_OK || fs_chown(r->path, r->uid, r->gid) != FS_OK) return false;
    if (r->protect && fs_set_protect(r->path, true) != FS_OK) return false;
    return true;
}

int fsimage_load(uint32_t *nodes) {
    ramdisk_info_t info; ramdisk_info(&info);
    if (!info.image_valid) return SYS_ENOENT;
    uint32_t total = info.image_bytes;
    if (total < HDR_BYTES || total > (uint32_t)RAMDISK_SECTORS * RAMDISK_SECTOR) return SYS_EINVAL;
    size_t sectors = (total + RAMDISK_SECTOR - 1) / RAMDISK_SECTOR;
    uint8_t *img = kcalloc(sectors, RAMDISK_SECTOR);
    if (!img) return SYS_ENOMEM;
    int rc = 0;
    for (size_t s = 0; s < sectors && rc == 0; s++) rc = ramdisk_read((uint32_t)s, img + s * RAMDISK_SECTOR);
    size_t payload = rc == 0 ? get32(img + 12) : 0;
    uint32_t count = rc == 0 ? get32(img + 8) : 0;
    if (rc == 0 && (HDR_BYTES + payload != total || fnv1a(img + HDR_BYTES, payload) != get32(img + 16) || !parse(img + HDR_BYTES, payload, count, NULL, NULL))) rc = SYS_EINVAL;
    if (rc == 0) {                                                 /* verified: now replace the live tree, acting as root */
        uint16_t su, sg; fs_get_user(&su, &sg); uint16_t sm = fs_get_umask();
        fs_set_user(0, 0);
        rc = fs_init();
        if (rc == FS_OK && !parse(img + HDR_BYTES, payload, count, apply_rec, NULL)) rc = SYS_EINVAL;
        fs_set_user(su, sg); fs_set_umask(sm);
        if (rc == 0 && nodes) *nodes = count;
    }
    kfree(img);
    return rc;
}
