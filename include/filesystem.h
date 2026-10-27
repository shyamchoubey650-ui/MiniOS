#ifndef FILESYSTEM_H
#define FILESYSTEM_H
/* MiniFS: a RAM-backed, tree-structured file system with Unix-like permissions.
 *
 * REAL:      files, directories, paths (. .. //), create/read/write/append/truncate, remove,
 *            rename/move, copy, recursive operations, permissions (rwx for owner/group/other),
 *            consistency checker.
 * VOLATILE:  everything lives in the kernel heap. There is NO persistence: contents are lost
 *            on reboot or shutdown (a disk driver is not part of this project phase).
 * NOT DONE:  hard/symbolic links, timestamps from a real-time clock (times are timer ticks),
 *            locking between tasks (only the shell uses MiniFS today). */
#include "include/types.h"

/* ---- limits ---- */
#define FS_NAME_MAX        31          /* characters per path component */
#define FS_PATH_MAX        128         /* characters per full path, including the NUL */
#define FS_MAX_DEPTH       24          /* path components */
#define FS_MAX_NODES       512         /* files + directories, including "/" */
#define FS_MAX_FILE_SIZE   (256u * 1024u)

/* ---- error codes ---- */
#define FS_OK                   0
#define FS_ERR_NOT_FOUND      (-1)
#define FS_ERR_EXISTS         (-2)
#define FS_ERR_NOT_DIR        (-3)
#define FS_ERR_IS_DIR         (-4)
#define FS_ERR_NOT_EMPTY      (-5)
#define FS_ERR_INVALID        (-6)    /* bad argument, bad name, empty path, "." / ".." as a name */
#define FS_ERR_NAME_TOO_LONG  (-7)
#define FS_ERR_PATH_TOO_LONG  (-8)    /* path or nesting depth over the limit */
#define FS_ERR_NO_SPACE       (-9)    /* node limit reached or out of kernel memory */
#define FS_ERR_TOO_BIG        (-10)   /* file would exceed FS_MAX_FILE_SIZE */
#define FS_ERR_PERMISSION     (-11)
#define FS_ERR_BUSY           (-12)   /* "/" or a directory that contains the working directory */
#define FS_ERR_INTO_SELF      (-13)   /* moving/copying a directory into itself */
#define FS_ERR_LOCKED         (-32)   /* another user holds the advisory lock (root and the lock owner may still change the file) */
#define FS_ERR_PROTECTED      (-30)   /* node has the protect flag (cannot be changed, moved or deleted) */

/* ---- permissions ---- */
#define FS_R 4
#define FS_W 2
#define FS_X 1
#define FS_MODE_MASK   0777
#define FS_DIR_MODE    0755
#define FS_FILE_MODE   0644

/* user ids used for the standard layout (the accounts themselves are managed by include/users.h) */
#define FS_UID_ROOT     0
#define FS_UID_ADMIN    1
#define FS_UID_STUDENT  2
#define FS_UID_GUEST    3

typedef enum { FS_FILE = 1, FS_DIR = 2 } fs_type_t;
typedef enum { FS_WRITE_TRUNCATE = 0, FS_WRITE_APPEND = 1 } fs_write_mode_t;

typedef struct {
    uint32_t  ino;
    fs_type_t type;
    uint16_t  mode;                 /* 9 permission bits */
    uint16_t  uid, gid;
    size_t    size;                 /* bytes (files) */
    uint32_t  children;             /* entries (directories) */
    bool      protect;              /* protect flag: blocks write, truncate, rename, delete */
    bool      locked;               /* advisory lock: blocks OTHER users (not root, not the lock owner) */
    uint16_t  lock_uid;             /* who holds the lock (valid when locked) */
    uint32_t  mtime, ctime;         /* timer ticks since boot */
    char      name[FS_NAME_MAX + 1];
} fs_stat_t;

typedef struct { char name[FS_NAME_MAX + 1]; fs_type_t type; size_t size; uint16_t mode; uint32_t ino; } fs_dirent_t;

typedef struct {
    uint32_t nodes, files, dirs, max_nodes;
    size_t   data_bytes;            /* sum of file sizes */
    size_t   capacity_bytes;        /* heap actually reserved for file data */
    size_t   node_bytes;            /* heap used by node records */
} fs_usage_t;

typedef struct { uint32_t nodes_seen, problems; char first[4][72]; } fs_check_report_t;

/* ---- path helpers (kernel/filesystem/path.c; pure functions) ---- */
bool        fs_name_valid(const char *name);                    /* 1..31 printable chars, no '/', not "." or ".." */
int         fs_path_normalize(const char *path, const char *cwd, char *out, size_t outsz);
int         fs_path_split(const char *canonical, char *parent, size_t psz, char *name, size_t nsz);
const char *fs_path_basename(const char *path);                 /* last component; expects no trailing slash (use a normalised path) */
int         fs_path_depth(const char *canonical);               /* components in an absolute canonical path */

/* ---- lifecycle ---- */
int  fs_init(void);                                             /* build (or rebuild) an empty file system with the standard layout */
void fs_set_clock(uint32_t (*ticks)(void));
void fs_set_user(uint16_t uid, uint16_t gid);                   /* credentials used for permission checks; uid 0 is root */
void fs_get_user(uint16_t *uid, uint16_t *gid);
/* The permission rule (pure): uid 0 may do anything; otherwise the owner, group or other triplet is used. */
bool fs_mode_allows(uint16_t mode, uint16_t owner, uint16_t group, uint16_t uid, uint16_t gid, int want);
void fs_set_umask(uint16_t mask);                               /* bits cleared from the default 0666 (files) / 0777 (directories); default 022 */
uint16_t fs_get_umask(void);

/* ---- queries ---- */
int  fs_stat(const char *path, fs_stat_t *out);
int  fs_getcwd(char *buf, size_t size);
int  fs_chdir(const char *path);
int  fs_resolve(const char *path, char *out, size_t outsz);     /* canonical absolute path using the working directory */
typedef bool (*fs_dirent_fn)(const fs_dirent_t *entry, void *ctx);
int  fs_readdir(const char *path, fs_dirent_fn fn, void *ctx);  /* sorted by name; the callback must not modify the tree */
typedef bool (*fs_walk_fn)(const char *path, const fs_stat_t *st, int depth, void *ctx);
int  fs_walk(const char *path, fs_walk_fn fn, void *ctx);       /* pre-order, sorted; includes the start node at depth 0 */
int  fs_tree_size(const char *path, size_t *bytes, uint32_t *files, uint32_t *dirs);
int  fs_can_write(const char *path);                            /* FS_OK if a write to `path` would be allowed (creating it if needed) */

/* ---- directories ---- */
int  fs_mkdir(const char *path);
int  fs_mkdir_parents(const char *path);                        /* like mkdir -p */
int  fs_rmdir(const char *path);                                /* empty directories only */
int  fs_remove_tree(const char *path, uint32_t *removed);       /* recursive; refuses "/" and the working directory's ancestors */

/* ---- files ---- */
int  fs_create(const char *path);                               /* new empty file; FS_ERR_EXISTS if present */
int  fs_touch(const char *path);                                /* create, or update the modification time */
int  fs_remove(const char *path);                               /* files only */
int  fs_write(const char *path, const void *data, size_t len, fs_write_mode_t mode);   /* creates the file if needed */
int  fs_read(const char *path, void *buf, size_t max, size_t offset, size_t *out_len);
int  fs_write_at(const char *path, size_t offset, const void *data, size_t len);   /* overwrite/extend an EXISTING file; gaps read as zeros */
int  fs_truncate(const char *path, size_t new_size);            /* growing fills with zero bytes */
int  fs_rename(const char *from, const char *to);               /* move/rename; `to` must not exist */
int  fs_copy(const char *from, const char *to);                 /* file to new or existing file */
int  fs_copy_tree(const char *from, const char *to);            /* file or directory tree to a NEW path */
int  fs_swap_data(const char *a, const char *b);                /* exchange the contents of two files */

/* ---- attributes ---- */
int  fs_chmod(const char *path, uint16_t mode);
int  fs_chown(const char *path, uint16_t uid, uint16_t gid);
int  fs_set_lock(const char *path, bool on);                     /* files only; lock: anyone who may write it; unlock: the lock owner or root */
int  fs_set_protect(const char *path, bool on);                  /* owner or root; guards one node (a directory's contents stay changeable) */

/* ---- accounting and checking ---- */
void fs_usage(fs_usage_t *out);
int  fs_check(fs_check_report_t *report);                       /* returns the number of problems found (0 = consistent) */
const char *fs_strerror(int code);
void fs_mode_string(fs_type_t type, uint16_t mode, char out[11]);   /* "drwxr-xr-x" */
#endif
