# MiniFS - the MiniOS file system (Phase 8)

## What is REAL and what is not

| Feature | Status | Notes |
|---|---|---|
| Files and directories as a tree | **REAL** | `kernel/filesystem/filesystem.c` |
| Absolute and relative paths, `.`, `..`, `//` | **REAL** | `path.c`, pure functions, unit-tested |
| create / read (with offset) / write / append / truncate | **REAL** | contents are heap buffers |
| remove, rmdir, recursive remove, rename/move, copy, recursive copy | **REAL** | all-or-nothing where it matters (see below) |
| Permissions: rwx for owner / group / other, root bypass | **REAL** | enforced on every operation |
| Consistency checker (`fsck`) | **REAL** | detects corrupted links, counters, ordering, leaks |
| `>` and `>>` shell redirection | **REAL** | output is captured, then written to the file |
| **Persistence** | **NOT IMPLEMENTED** | MiniFS lives in RAM only: everything is lost on reboot/shutdown |
| Disk driver, block layer, journaling | **NOT IMPLEMENTED** | `freeblocks` says so; there are no blocks |
| Hard/symbolic links | **NOT IMPLEMENTED** | |
| Real-time timestamps | **NOT IMPLEMENTED** | times are timer ticks since boot |
| Pipes (`a | b`) | **IMPLEMENTED (Phase 13)** | stages run one after another, output goes through a 32 KiB buffer; see [PIPES.md](PIPES.md) |
| File locking between tasks | **NOT IMPLEMENTED** | only the shell uses MiniFS today; a lock arrives with the system-call layer (Phase 9) |
| Users and logins | PLANNED (Phase 10) | permissions already work with numeric user ids |

## Standard layout (created by `fs_init`)

```
/
|-- bin/  etc/  tmp/(0777)  usr/
|-- etc/motd  etc/hostname          (real files with real content)
|-- home/ admin/(uid 1, 0750)  student/(uid 2, 0750)
`-- var/  log/
```

## Data structures

```c
struct fs_node {                 /* one per file or directory, allocated with kmalloc */
    uint32_t ino; fs_type_t type; uint16_t mode, uid, gid; uint32_t mtime, ctime;
    char name[32];
    fs_node *parent, *first_child, *next_sibling;   /* children are kept SORTED by name */
    uint32_t child_count;
    uint8_t *data; size_t size, capacity;           /* files: growable buffer */
};
```

Limits: 512 nodes, 31-character names, 128-character paths, 24 levels of nesting, 256 KiB per file.
Every limit has its own error code; no operation silently truncates or half-completes.

## Design rules that the tests check

* **Atomic writes.** `fs_write`/`truncate`/`copy` allocate the new buffer first. If memory or the size limit stops them, the old content is untouched, and a file that was just created for the write is removed again.
* **All-or-nothing recursion.** `fs_remove_tree` checks permissions for the whole subtree before deleting anything; `fs_copy_tree` checks node space and permissions first and rolls back a partial copy if memory runs out mid-way.
* **No dangling working directory.** A directory that is, or contains, the working directory cannot be removed (`FS_ERR_BUSY`). A renamed directory carries the working directory with it.
* **Moves keep the node.** `mv` relinks the same node, so data is not copied and nothing can be lost half-way. Moving a directory into itself is refused.
* **Permissions.** Traversal needs `x` on each directory, reading needs `r`, writing needs `w`; creating or deleting needs `w`+`x` on the parent. No sticky bit: anyone with write access to `/tmp` may delete files in it. Root ignores the bits. Only the owner (or root) may `chmod`; only root may `chown`.
* **Recursion is bounded** by the 24-level depth limit, so the 16 KiB shell stack is never at risk.

## Redirection (`shell/redirect.c`, `kernel/device/capture.c`)

`cmd > file` / `cmd >> file`:
1. the target is validated **before** the command runs (parent exists, not a directory, writable);
2. the command's screen output is captured into a 32 KiB buffer (only output of the shell task: a background `ticker` still prints normally);
3. if the command succeeded its output is written to the file; if it failed, its error message is shown on screen and the file is left alone; output over 32 KiB is refused rather than truncated.

Because commands print errors to the same stream, a failing `cat missing > f` shows the error and does not create `f` (a real shell would create an empty file).

## Commands (63 new, all real)

| Group | Commands |
|---|---|
| Navigation | `pwd cd home up pushd popd dirs` |
| Listing | `ls lsdirs lsfiles lsr dircount tree find` (`ls -l -a`, `find` takes `*` and `?`) |
| Directories | `mkdir [-p] mkpath rmdir rmtree cpdir isdir` |
| Files | `touch write append note rm cp [-r] mv rename stat file truncate mkfile exists filesize readfile backup restore swapfiles fill touchn` |
| Safety | `trash undelete emptytrash shred` (`/tmp/.trash`) |
| Viewing | `cat head tail wc grep hexdump cmp motd` (`grep` is plain text, not regex) |
| Info/attributes | `df du mkfs fsck inodes freeblocks fsstat fsdump chmod readonly writable` |

`chmod` accepts octal (`644`) and symbolic modes (`+x`, `go-w`, `u=r`). `mkfs` needs `--yes`.
Still stubs: `chown`, `chgrp` and other user-dependent commands (Phase 10), `pipe`-style commands, `fdlist`/`fdinfo` (Phase 9).

## Error messages

```
MiniOS> cd unknown        Error: directory 'unknown' does not exist.
MiniOS> rm missing.txt    Error: file 'missing.txt' not found.
```
Internally every function returns an `FS_ERR_*` code; `cu_fs_error()` turns it into the message.

## Testing

| File | Checks |
|---|---|
| `tests/test_fs.c` | path normalisation boundaries (127/128 chars, 31/32-char names), layout, mkdir/-p, depth limit, file I/O, size limits, rename/copy/tree operations, node limit, **out-of-memory behaviour**, permissions (owner/group/other/root), walk order, fsck against deliberately corrupted structures, 4000-operation randomized stress that must return the heap to its starting state |
| `tests/test_fs_commands.c` | the real commands through the real parser, registry and redirection: the spec's demo sequence, every error message, edge cases of `>`/`>>` |
| `tests/test_text.c` | wildcard matching, head/tail/wc/grep helpers, output capture |

Mutation checks: deliberately breaking the file system (unsorted insert, no into-self check, no emptiness check, ignored permissions, unchecked limits, missing rollback, ...) and the redirection code makes these tests fail.

## Limitations

* Volatile: nothing survives a reboot.
* 512 nodes, 256 KiB per file, 32 KiB per redirected output.
* One shared working directory for the whole system (per-process directories come with Phase 9/10).
* `cp`/`mv` of a name that exists as a directory always means "into that directory".

## Advisory locks (Phase 12)
`lock <file>` marks a file so that OTHER users get `EFLOCK` when they write, truncate, move or delete it. The lock owner and root are not
blocked. Only files can be locked, only by someone who could write them, and only the owner or root can unlock. Locks are runtime state:
`savefs` does not store them and `loadfs` clears them; deleting the file removes its lock. `stat` shows `Lock   : held by <user>`.
