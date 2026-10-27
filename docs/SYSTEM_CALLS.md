# MiniOS system calls (Phase 9)

## What is REAL and what is not

| Feature | Status | Notes |
|---|---|---|
| `int 0x80` trap gate, register convention, dispatch table (37 calls) | **REAL** | `boot/interrupts.S` (stub), `kernel/cpu/idt.c` (gate), `kernel/syscall/syscall.c` |
| Argument validation (NULL, wrap-around, RAM bounds, bounded strings) | **REAL** | every pointer is checked before the kernel touches it |
| Write protection of kernel code/read-only data against call arguments | **REAL** | `memory_set_code_range()`; a call cannot be told to write there |
| Per-process file descriptors (13 + stdin/stdout/stderr) | **REAL** | stale-file detection by inode |
| Per-process allocation tracking, released when the process dies | **REAL** | killed or crashed processes cannot leak |
| One error-code space, translated from MiniFS / process / heap errors | **REAL** | `sys_strerror`, `errno` command |
| Call statistics (per call: count and failures) | **REAL** | `sysinfo`, `sys_stats()` |
| User mode (ring 3), real memory protection | **NOT IMPLEMENTED** | all tasks run in ring 0 and share memory. The gate already has DPL 3, ready for it |
| Per-process working directory and per-process user credentials | **NOT IMPLEMENTED** | one shared working directory; permissions use one global user (Phase 10) |
| Tracing (`strace`/`trace`) | **REAL** | `sudo trace on` logs `name(args) = result` for every call ([LOGGING.md](LOGGING.md)) |

**What validation can and cannot do.** It stops wild, NULL, wrapped-around and out-of-RAM pointers, and
refuses writes into kernel code. It cannot stop a hostile task from passing a pointer into kernel *data*,
because without paging every address is valid. That needs ring 3 + page tables (a future phase).

## Calling convention (i386, vector 0x80)

```
eax = call number      ebx, ecx, edx, esi = arguments 1..4
eax = result: >= 0 success value, < 0 error code (SYS_E*)
```
`isr128` pushes the same frame as every interrupt; `syscall_interrupt()` reads the arguments from the saved
registers and stores the result in the saved `eax`, so the `iret` path returns it. System calls run with
interrupts disabled (interrupt gate), except while a call blocks (wait, sleep, read from the keyboard).
User-side wrappers (`lib/usyscall.c`) hide the trap: `sys_open(path, flags)`, `sys_write(fd, buf, n)`, ...

## The calls

| # | Call | Arguments | Returns |
|---|---|---|---|
| 0 | `sys_process_exit` | code | does not return (shell/idle: `EPERM`) |
| 1,2 | `sys_getpid`, `sys_getppid` | | pid |
| 3 | `sys_yield` | | 0 |
| 4 | `sys_sleep` | milliseconds (max 1 h) | 0 |
| 5 | `sys_process_create` / `_named` | program, arg, priority [, display name] | new pid |
| 6 | `sys_process_kill` | pid | 0 |
| 7 | `sys_process_wait` | pid (-1 = any child), &exit_code | reaped pid |
| 8 | `sys_process_info` | pid, &info | 0 |
| 9 | `sys_process_list` | pids[], max (NULL,0 = count) | count (sorted by pid) |
| 10,11 | `sys_process_suspend` / `_resume` | pid | 0 |
| 12 | `sys_setpriority` | pid, 1-20 | 0 |
| 13 | `sys_write` | fd, buf, n | bytes written (fd 1,2 = screen) |
| 14 | `sys_read` | fd, buf, n | bytes read (fd 0 = keyboard, no echo) |
| 15 | `sys_open` | path, flags | descriptor (3..15) |
| 16 | `sys_close` | fd | 0 |
| 17 | `sys_lseek` | fd, offset, SET/CUR/END | new offset |
| 18,19 | `sys_create`, `sys_delete` | path | 0 |
| 20,21 | `sys_mkdir` (flag: parents), `sys_rmdir` | path | 0 |
| 22 | `sys_rename` | from, to | 0 |
| 23,24 | `sys_stat`, `sys_fstat` | path / fd, &stat | 0 |
| 25 | `sys_readdir` | path, entries[], max (NULL,0 = count) | entries |
| 26,27 | `sys_getcwd`, `sys_chdir` | | length / 0 |
| 28,29,30 | `sys_chmod`, `sys_touch`, `sys_truncate` | | 0 |
| 31,32 | `sys_memory_alloc`, `sys_memory_free` | size / pointer | pointer / 0 |
| 33,34 | `sys_sysinfo`, `sys_time` | | summary / ticks |
| 35 | `sys_fdinfo` | fd, &info | 0 |
| 36 | `sys_stats` | &stats | 0 |

Open flags: `SYS_O_READ`, `WRITE`, `CREATE`, `TRUNC`, `APPEND`, `EXCL`. `TRUNC`/`APPEND` need `WRITE`; unknown bits are rejected.

## Error codes

`ENOENT EEXIST ENOTDIR EISDIR ENOTEMPTY EINVAL ENAMETOOLONG EPATHTOOLONG ENOSPC EFBIG EACCES EBUSY EINTOSELF`
(-1 to -13, **numerically equal to the MiniFS errors**, so file-system results pass through unchanged), then
`ESTALE EBADF EMFILE EFAULT ENOSYS ESRCH ECHILD EINTR ENOMEM EPERM EAGAIN ELIMIT ESTATE` (-14 to -26), and from Phase 10
`EAUTH ELOCKED ENOUSER` (-27 to -29). Calls 37-53 (identity and accounts) are described in [SECURITY.md](SECURITY.md). Phase 11 adds calls 54-61 (`kinfo power protect blk_info blk_read blk_write fs_save fs_load`) and the errors `EPROTECT` (-30) and `EBLOCK` (-31); raw block access, power control and image save/load need effective uid 0.
`errno <code|NAME>` explains any of them.

## File descriptors

* A descriptor stores the **absolute path, the inode number, flags and the offset**. MiniFS is path-based, so before every
  read/write/seek the kernel re-resolves the path and checks the inode: if the file was deleted, renamed, or replaced by a new
  one with the same name the call fails with `ESTALE` instead of touching the wrong file.
* Descriptors belong to one process; another process using the number gets `EBADF`. They are closed when the process ends.
* Writes go through `fs_write_at` (overwrite or extend; gaps read as zero bytes). `O_APPEND` moves to the end before each write.
* Descriptors 0-2 are the console. Output written to 1 or 2 passes through the screen driver, so shell redirection captures it.

## Memory

`sys_memory_alloc` returns heap memory tracked per process (max 48 live allocations, `ELIMIT` beyond that). Only the owner can
free it (`EINVAL` otherwise). Everything still allocated when a process exits is freed by the exit hook, together with its
descriptor table.

## Which commands use system calls

**73 commands use system calls only** (verified by searching the source for direct kernel calls):

* directories: `pwd cd home up pushd popd dirs ls lsdirs lsfiles dircount mkdir mkpath rmdir isdir`
* files: `touch write append note rm mv rename stat file truncate mkfile exists filesize readfile fill touchn trash undelete shred`
* text: `cat head tail wc grep hexdump cmp motd` (they load files with open/read/close into a `sys_memory_alloc` buffer)
* attributes: `chmod readonly writable`; memory: `alloc dealloc`
* processes: `ps spawn kill wait pstree pid ppid nice renice priority suspend resume yield jobs pinfo pkill killall pgrep proccount procdemo`
* new in Phase 9: `sysinfo uname hostname errno fdlist fdinfo`

**Still calling kernel APIs directly (PARTIAL), and why:**

| Commands | Reason |
|---|---|
| `tree find lsr rmtree cpdir cp backup restore swapfiles emptytrash` | whole-tree walks and copies need the file system's atomic recursive operations; there is no `walk`/`copy` system call yet |
| `df du mkfs fsck inodes freeblocks fsstat fsdump` | file-system diagnostics and administration |
| `mem free meminfo memmap heap blocks memstat memfrag memtest memdump memfill pages peek poke` | memory diagnostics that read allocator internals |
| `sched quantum setquantum runqueue ctxstat top` | scheduler views (ready queue, time slice) are not exposed |
| shell built-ins, `>` / `>>` redirection | part of the shell itself |

## Testing

`tests/test_syscall.c` (191 checks) runs the real dispatcher, process manager, MiniFS and heap:
unknown calls, **every call's success and failure paths**, NULL / unreadable / wrapped / read-only-region pointers,
strings without a terminator, descriptor semantics (modes, seek, append, truncate, sparse writes, limit of 13),
**stale descriptors after delete/rename/replace**, permission failures through the call layer, per-process isolation of
descriptors and allocations, cleanup when a process is killed or exits, blocking calls (wait, sleep) and the exact
`int 0x80` register convention. Mutation checks (no validation, no stale check, no cleanup, no limits, ...) make it fail.

The migrated commands are covered by the Phase 6-8 command suites, which now run them **through the real syscall layer**
(on a static low-address stack so the 32-bit validators see valid addresses). One test runs a batch of commands and
verifies that the heap and descriptor table end exactly where they started (no leaks).

At boot the kernel executes a real `int 0x80` self-test: `getpid`, open/read/close of `/etc/hostname`, and a deliberately bad pointer.

## Limitations

* Ring 0 only; no protection against a malicious task (see above).
* A call that blocks inside the kernel holds the caller's stack; each process has 8 KiB.
* One shared working directory and one global set of credentials.
* `read(0)` returns raw key codes without echo or line editing; arrow keys are dropped.
* Preemption does not happen at the end of a system call, only at timer interrupts.

## Phase 12
Adds calls 62-64: `log_get log_ctl lock`, and the error `EFLOCK` (-32, file locked by another user). `power` gains operations 4 and 5 (PC speaker on/off) which any user may use; reboot, shutdown and halt still need root. Reading the log is open to everybody (private sources are root only); changing it needs root.
