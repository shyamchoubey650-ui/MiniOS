/* Path handling for MiniFS. Pure string functions: no kernel dependencies, so they are
 * unit-tested on the host. Paths are normalised lexically ("." and ".." are resolved
 * textually; MiniFS has no symbolic links, so that is exact). ".." at the root stays at the root. */
#include "include/filesystem.h"

bool fs_name_valid(const char *name) {
    size_t n = 0;
    if (!name || !*name) return false;
    if ((name[0] == '.' && name[1] == '\0') || (name[0] == '.' && name[1] == '.' && name[2] == '\0')) return false;
    for (; name[n]; n++) {
        unsigned char c = (unsigned char)name[n];
        if (c == '/' || c < 32 || c > 126) return false;
        if (n >= FS_NAME_MAX) return false;
    }
    return true;
}

/* Appends `comp` (length n) to the canonical path in `out` (current length *len). */
static int push(char *out, size_t outsz, size_t *len, const char *comp, size_t n) {
    if (n > FS_NAME_MAX) return FS_ERR_NAME_TOO_LONG;
    if (*len == 1) *len = 0;                       /* path is "/": do not produce "//" */
    if (*len + 1 + n + 1 > outsz) { if (*len == 0) *len = 1; return FS_ERR_PATH_TOO_LONG; }
    out[(*len)++] = '/';
    for (size_t i = 0; i < n; i++) out[(*len)++] = comp[i];
    out[*len] = '\0';
    return FS_OK;
}

static void pop(char *out, size_t *len) {
    if (*len <= 1) return;                         /* ".." at the root stays at the root */
    while (*len > 1 && out[*len - 1] != '/') (*len)--;
    if (*len > 1) (*len)--;                        /* drop the slash itself */
    out[*len] = '\0';
    if (*len == 0) { out[0] = '/'; out[1] = '\0'; *len = 1; }
}

int fs_path_normalize(const char *path, const char *cwd, char *out, size_t outsz) {
    if (!path || !*path || !out || outsz < 2) return FS_ERR_INVALID;
    size_t len = 1;
    out[0] = '/'; out[1] = '\0';

    const char *p = path;
    if (path[0] != '/') {                          /* relative: start from the working directory */
        if (!cwd || cwd[0] != '/') return FS_ERR_INVALID;
        int rc = fs_path_normalize(cwd, "/", out, outsz);
        if (rc != FS_OK) return rc;
        for (len = 0; out[len]; len++) {}
    }
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - start);
        if (n == 1 && start[0] == '.') continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') { pop(out, &len); continue; }
        int rc = push(out, outsz, &len, start, n);
        if (rc != FS_OK) return rc;
        for (size_t i = 0; i < n; i++) {           /* control characters are never valid in names */
            unsigned char c = (unsigned char)start[i];
            if (c < 32 || c > 126) return FS_ERR_INVALID;
        }
    }
    return FS_OK;
}

int fs_path_depth(const char *canonical) {
    int d = 0;
    if (!canonical) return 0;
    for (size_t i = 0; canonical[i]; i++) if (canonical[i] == '/' && canonical[i + 1]) d++;
    return d;
}

int fs_path_split(const char *c, char *parent, size_t psz, char *name, size_t nsz) {
    if (!c || c[0] != '/' || !parent || !name) return FS_ERR_INVALID;
    size_t len = 0, slash = 0;
    while (c[len]) { if (c[len] == '/') slash = len; len++; }
    if (len == 1) return FS_ERR_INVALID;           /* "/" has no parent or name */
    size_t nlen = len - slash - 1;
    if (nlen == 0 || nlen >= nsz || nlen > FS_NAME_MAX) return FS_ERR_NAME_TOO_LONG;
    size_t plen = slash == 0 ? 1 : slash;
    if (plen + 1 > psz) return FS_ERR_PATH_TOO_LONG;
    for (size_t i = 0; i < plen; i++) parent[i] = c[i];
    parent[plen] = '\0';
    for (size_t i = 0; i < nlen; i++) name[i] = c[slash + 1 + i];
    name[nlen] = '\0';
    return FS_OK;
}

const char *fs_path_basename(const char *path) {
    const char *base = path;
    if (!path) return "";
    for (const char *p = path; *p; p++) if (*p == '/' && p[1] && p[1] != '/') base = p + 1;
    return base;
}
