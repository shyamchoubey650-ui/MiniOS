# The kernel log (Phase 12)

## What it is
A fixed ring buffer of 256 entries inside the kernel (`include/logger.h`, `kernel/log/logger.c`). Every entry has a
sequence number, the timer tick, the process id that wrote it (-1 = kernel or interrupt context), a level
(`DEBUG INFO WARN ERROR`), a source tag and a message of up to 71 characters. When the ring is full the oldest entry is
overwritten (and counted: `logstats`). Nothing is allocated, and writers are interrupt-safe, because the scheduler logs
from the timer interrupt.

## What gets logged
| Source | What | Level |
|---|---|---|
| `boot` | every boot message (also copied to a separate boot snapshot, so `dmesg` survives a full ring) | INFO / ERROR |
| `sec` | logins, failed logins, `su`, `sudo`, denied actions, password changes, account changes (from the audit trail) | INFO, WARN for failures |
| `proc` | process created, killed, exited with a code, died from a CPU exception, stack overflow | INFO / ERROR |
| `cpu` | a faulting process, kernel panic | ERROR |
| `fs` | `mkfs` | WARN |
| `disk` | `savefs`, `loadfs` (success and failure) | INFO / WARN / ERROR |
| `log` | `loglevel`, `logclear`, `trace`, `schedtrace` were changed, and by which uid | INFO / WARN |
| `shell` | the NAME of each command that ran and its exit status (never the arguments: some commands take passwords) | DEBUG |
| `trace` | one line per system call while `trace` is on | DEBUG |
| `sched` | one line per context switch while `schedtrace` is on | DEBUG |

The boot-time self-tests of the user manager are muted on purpose: they fail logins deliberately and must not look like an attack.

## Levels
`loglevel` sets the MINIMUM level that is stored (default INFO, so `shell` entries are not stored unless you ask for DEBUG).
Entries below it are refused and counted. Trace entries ignore the level: asking for a trace means you want it.
`logs -l warn` only filters what is SHOWN.

## Who may do what
| Action | Needs |
|---|---|
| `logs dmesg logtail logfind logstats strace` | anybody, but entries from the private sources `sec` and `shell` are shown to root only |
| `loglevel <level>` `logclear` `logsave` `trace on/off` `schedtrace on/off` | root (`sudo`) |

Clearing the log writes a WARN entry saying who cleared it, and the sequence numbers keep counting, so a gap is visible.
The boot snapshot is not affected by `logclear`.

## Trace
`sudo trace on` records `name(args) = result`, for example `open(4096, 1) = -1 ENOENT`. Arguments are numbers (pointers print in
hex); file names are not recorded. The log calls themselves are never traced, otherwise reading the trace would add to it.
`strace <pid>` shows the entries of one process. Only the newest 256 entries exist, so turn the trace off when you are done.
`sudo schedtrace on` records `pid A -> pid B 'name' (time slice over | yielded or blocked)`.

## System calls
`SYS_LOG_GET (62)`: `sys_log_get(which, index, out)`, `which` = `SYS_LOG_RING` or `SYS_LOG_BOOT`; `out == NULL` returns the number of
entries the caller may see. `SYS_LOG_CTL (63)`: `sys_log_ctl(op, arg, out)` with `SYS_LOGCTL_STATS / SET_LEVEL / CLEAR / TRACE / SCHED`.

## Limits
256 entries, 71-character messages, RAM only (use `logsave` for a file), single console. Control characters are replaced by `?`
so a message can never move the cursor or clear a screen.

## Tests
`tests/test_logger.c` (54 unit checks: ring, wrap, levels, privacy, boot snapshot, trace bypass, formatter) and
`tests/test_phase12.c` (the log inside the real kernel code, all eleven commands, privacy, trace, and the 18 finished commands).
