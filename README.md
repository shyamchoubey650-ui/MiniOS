# MiniOS - Phase 16 (a better console)

Phase 16 improves the text console: **scrollback**, **Tab completion**, **cursor editing inside the line**, and the **current folder in the prompt**.
Build: `make`, run: `make run-kernel`, test: `make test` (22 host suites).

## What you can do now
| Keys | What happens |
|---|---|
| **PageUp / PageDown** | scroll back through the last 200 lines that left the screen (a blue bar on the top row says so). Any other key returns to the prompt |
| **Ctrl+Up / Ctrl+Down** | scroll back / forward 3 lines at a time (for keyboards without a PageUp key); also works at the login prompt |
| **Mouse wheel** | scrolls the console 3 lines per notch, at the shell and at the login prompt (needs a PS/2 mouse with a wheel: QEMU provides one, but only after its window has **captured the mouse**: click inside the window, release with Ctrl+Alt+G, or tick **View > Grab On Hover**) |
| **Tab** | completes the word before the cursor: **commands** (and if/for/while...), **$variables**, **file and directory names**. One match completes it, several complete as far as they agree, pressing Tab again shows the list |
| **Left / Right / Home / End / Delete** | move inside the line and edit it where you are, no more retyping. Ctrl+A / Ctrl+E also go to the start / end |
| **Ctrl+U / Ctrl+K / Ctrl+W** | cut the whole line / from the cursor to the end / the word before the cursor |
| **Up / Down** | history (as before; now the cursor ends at the end of the recalled line) |
| prompt | `admin@MiniOS:~>` shows where you are: `~` is your home, `~/docs` is inside it, a long path becomes `.../a/b`. A custom `PS1` still wins |

Tab details: after `;`, `|`, `&&`, `||`, `then`, `do`, `else`, `sudo`... it completes a command; after `>` or `<` or a command it completes a file; a word with `$` completes
variables (`${NAM` becomes `${NAME}`). Directories get a `/` and no space, files a space. Hidden files (starting with `.`) only appear if you type the dot. Names with spaces
or characters such as `; | & < > $ " '` are never offered (they would break the line). You are only offered what you may list: a directory you cannot read gives nothing.
Full description: [docs/CONSOLE_UI.md](docs/CONSOLE_UI.md).

## Honest limits of Phase 16
- Not run in QEMU by the author of this phase: the logic (edit buffer, completion, scrollback window, prompt path) has 318 host checks, and the keyboard, screen and shell wiring
  builds without warnings but only runs on the real kernel. Please try every key above, especially PageUp after `logs -a`, Tab on `/et` and editing in the middle of a long line
  that wraps onto a second row.
- The mouse driver (PS/2 controller, IRQ12, IntelliMouse wheel mode) is hardware code that could not be run here; only the packet decoder is tested (39 checks). Buttons and movement are decoded and counted but not used; USB mice and QEMU's `usb-tablet` are not supported.
- Scrollback keeps 200 lines and shows only what scrolled off the top. Lines already on screen when you press PageUp are included; text you `clear` is kept too.
- While you are looking at the history nothing new is drawn on screen; the next key (or any output) returns you to the live screen.
- Tab completes one word: no completion inside quotes, no `~` expansion, no completion of options (`-l`) or of the arguments of `set`/`alias`.
- The editor handles lines up to 127 characters, wrapping over rows; the line is redrawn from its start after an edit in the middle.
- The mouse is not used: MiniOS has no mouse driver.

# Phase 15 (previous phase): control flow


Phase 15 adds **if / elif / else / fi**, **for ... in ... do ... done**, **while ... do ... done**, **break** and **continue** to the shell,
on top of the command lists (Phase 14) and pipes (Phase 13). Build: `make`, run: `make run-kernel`, test: `make test` (19 host suites).

## Control flow in one minute
    admin@MiniOS> if test -f notes.txt ; then echo found ; else echo missing ; fi
    admin@MiniOS> for f in a.txt b.txt c.txt ; do wc -l $f ; done
    admin@MiniOS> for i in 1 2 3 4 5 ; do test $i = 4 && break ; echo $i ; done
    admin@MiniOS> while test -f /tmp/lock ; do echo waiting ; sleep 1 ; done
    admin@MiniOS> for n in pear apple ; do cat names.txt | grep $n | wc -l ; done

- **Condition = any command list.** It is true when its status is 0: `if cat f | grep x && test -d d ; then ...`. `test` and `true`/`false` are the usual helpers.
- **`if`** takes the first branch whose condition is true; with no branch taken its status is 0. **`for`** walks the words after `in` (variables are
  expanded, quotes keep spaces together, at most 28 words); the variable keeps its last value. **`while`** repeats while its condition succeeds.
- **`break`** leaves the innermost loop, **`continue`** jumps to its next turn; both work inside an `if` inside a loop, and after `&&`/`||`.
- **Several lines.** At the prompt an unfinished block shows a `> ` prompt until the closing `fi`/`done` (Ctrl+C cancels). A script run with `source` may
  also spread a block over lines (`for x in a b` / `do` / `echo $x` / `done`); `#` comments and blank lines inside a block are fine.
- A keyword counts only as the **first word of a command**: `echo if then fi` prints those words.
- The whole line is **checked before anything runs**: a missing `fi`/`done`, a stray `then`/`do`, an empty block, `fi` where `done` belongs, or `break`
  outside a loop is a syntax error (status 2) and nothing is executed.
- **Safety limits:** a loop stops after **1000 turns**, one line runs at most **5000 commands**, and **Ctrl+C** stops a loop (status 130).
- Only commands that really ran are written to the kernel log (names only); keywords are never logged.
Full description: [docs/CONTROL_FLOW.md](docs/CONTROL_FLOW.md).

## Honest limits of Phase 15
- Not run in QEMU by the author of this phase: 164 parser checks and 142 end-to-end checks pass on the host and the kernel builds without
  warnings, but please try the examples above on the real kernel, including the multi-line `> ` prompt and Ctrl+C in a loop.
- A block cannot be piped or redirected as a whole (`done | sort`, `fi > f` are errors); put the pipe or `>` on a command inside it.
- No `$(...)`, no arithmetic (use `calc` and files), no `case`, no functions, no parentheses.
- A command line holds at most 16 commands, so very deep nesting does not fit in one line (seven levels do); `source` has the same limit per statement.
- While a loop runs the keyboard is only read for Ctrl+C: other keys typed meanwhile are discarded.

# Phase 14 (previous phase): command lists

Phase 14 adds **command lists**: `a ; b`, `a && b`, `a || b`, around the pipelines of Phase 13. Build: `make`, run: `make run-kernel`,
test: `make test`.

## Command lists in one minute
    admin@MiniOS> mkdir work ; cd work ; pwd
    admin@MiniOS> test -f notes.txt && echo found || echo missing
    admin@MiniOS> cat names.txt | sort > sorted.txt && cat sorted.txt | head -n 2
    admin@MiniOS> set X=5 ; echo $X ; false ; echo $?

- `a ; b` runs `b` whatever happened. `a && b` runs `b` only if `a` succeeded (status 0). `a || b` runs `b` only if `a` failed.
- Read **left to right with equal priority**: `false && echo A || echo B` prints B; `true || echo A && echo B` prints B (not A).
- A skipped command leaves the status as it was, so `false && echo x ; echo $?` prints 1. The status of the line is the status of the last
  command that ran. An unknown command has status 127.
- Every command is alias-expanded, `$variable`-expanded and parsed **on its own just before it runs**, so `set X=1 ; echo $X` and `false ; echo $?` work.
- A pipeline is one command of a list: `cat f | sort > g && echo done`.
- Up to **8 commands** per line. `;`, `&&` and `||` inside quotes are plain text. A trailing `;` is fine (`echo hi ;`).
- Syntax errors (`a ;; b`, `&& a`, `a &&`, a lone `&`, an unterminated quote) are reported before anything runs.
  A lone `&` means "background job", which MiniOS does not support: use `spawn`.
- A syntax error inside ONE command (`echo hi >`) fails only that command: earlier ones already ran, and `&&`/`||` see the failure.
- Scripts run by `source` can use lists too. `wc` no longer prints `-` after the numbers when it reads a pipe, so `cat f | wc -l | sum` works.

## Honest limits of Phase 14
- Not run in QEMU by the author of this phase: 135 parser checks and 79 end-to-end checks pass on the host, the kernel builds without
  warnings, but please try the examples above on the real kernel.
- No parentheses, no `if`/`for`/`while`, no `$(...)`, no background `&`: a list is a flat sequence of up to 8 commands.
- Aliases and `$variables` are expanded for each command separately, but an alias is still only expanded for the first word of a command.

# Phase 13 (previous phase): pipes
Phase 13 added **pipelines**: `cmd1 | cmd2 | cmd3`, optionally ending in `> file` or `>> file`.

## Pipes in one minute
    admin@MiniOS> cat notes.txt | sort | uniq -c | sort -n
    admin@MiniOS> cat scores.csv | cut -d , -f 2 | avg
    admin@MiniOS> logs -a | grep proc | wc -l
    admin@MiniOS> cat names.txt | tr a-z A-Z > upper.txt
    admin@MiniOS> logs -a | more                    (the last stage may page, because it is not captured)

- Up to **4 commands** per line. Only the **last** may use `>` or `>>`. A `|` inside quotes is ordinary text.
- The stages run **one after another**, not at the same time: MiniOS has no stdin/stdout streams. The output of a stage is
  collected (at most **32 KiB**) and handed to the next one as the file name `-`. The shell adds the `-` for you for:
  `cat head tail wc grep sort uniq cut tr rev nl tac fold join wordfreq hexdump more` (as `... -`) and
  `upper lower capitalize sum min max avg` (as `... -f -`). Any other command can be given `-` by hand: `cat a.txt | diff b.txt -`.
- Everything is checked **before** the first stage runs: an unknown command, a command that cannot read a pipe (`ls`, `pwd`),
  an unwritable `>` target. A typo in the last command never leaves the first ones executed.
- If a stage **fails** (it prints `Error:` or `usage:`), its message is shown and the pipeline stops, so an error message never
  becomes data for the next command. A stage that only finds nothing (`grep` with no match, status 1) is not a failure.
- `sudo`, `su`, `login`, `logout`, `passwd` and `exit` cannot be in a pipeline (their password prompts would be captured).
- The exit status (`$?`) is the status of the last command.
- A pipe does not bypass permissions: every command still reads through the system calls as the current user.
- New: `sum`, `min`, `max`, `avg` accept `-f <file>` (numbers separated by spaces, commas or newlines).
Full description: [docs/PIPES.md](docs/PIPES.md).

## Honest limits of Phase 13
- Not run in QEMU by the author of this phase: the pipeline code is covered by 237 host checks plus 94 parser checks, and
  it builds without warnings, but please try the examples above on the real kernel.
- Stages are not concurrent: a stage finishes completely before the next one starts, so `yes | head` cannot work and a
  long-running first stage delays the rest.
- Only commands that load their input through the shared file loader can read a pipe. `upper`, `lower`, `capitalize` and the number
  commands take `-f -`; `contains`, `replace`, `strlen` and friends take their text as arguments and cannot read a pipe.
- Aliases are expanded only for the first word of the whole line, so `cat f | myalias` does not expand `myalias`.
- A pipeline cannot run inside another captured stage (for example `source script.sh | cat` when the script itself contains a pipe).

## Phase 12 (previous phase): the kernel log and all 300 commands
| Area | Commands | Status |
|---|---|---|
| Kernel log | logs dmesg loglevel logclear logsave logtail logfind logstats | REAL: a 256-entry ring buffer (time, level DEBUG/INFO/WARN/ERROR, source, process id, message). Boot messages, logins, failed logins, process start/exit/crash, `mkfs`, `savefs`, `loadfs` and log changes are recorded. Read it with `dmesg` (boot) and `logs`; `logtail -f` follows it live |
| Tracing | trace strace schedtrace | REAL: `sudo trace on` logs every system call (`open(...) = -1 ENOENT`), `strace <pid>` filters one process, `sudo schedtrace on` logs every context switch |
| File locks | lock unlock | REAL, enforced by MiniFS: while a file is locked, OTHER users cannot write, truncate, move or delete it; the lock owner and root can. `protect` still blocks everybody |
| Accounting | quota memlimit | `quota` REAL usage per user, limits are DISPLAY ONLY; `memlimit` records a per-process limit and reports "OVER", it stops nothing |
| Jobs | bg fg | REAL on the shell's own children: `bg` resumes a suspended job, `fg` resumes it and waits for it (Ctrl+C stops waiting). There is no Ctrl+Z |
| Loopback | loopback nc | REAL in-memory message queue between tasks: `loopback demo` starts an echo task (`lbecho`) and exchanges messages with it. `nc localhost <port> <text>` and `nc -l <port>` use the same queue. Receiving on ports below 1024 needs root |
| Network (simulated) | telnet ssh ftp whois | **SIMULATED**, every output labelled `[SIMULATED]`: nothing is connected, no file is transferred |
| Disk | mount umount defrag smart | `mount`/`umount` keep a TABLE only (nothing is attached); `defrag` and `smart` are labelled simulations built on real heap and RAM-disk numbers |
| Devices | beep clock | REAL: PC speaker through PIT channel 2 (QEMU is silent without an audio backend), live CMOS clock until Ctrl+C or `q` |

## What the log shows (try it)
    admin@MiniOS> dmesg                      the boot messages
    admin@MiniOS> logs -l warn               only warnings and errors
    admin@MiniOS> sudo trace on              then do something, then:  logs -s trace  /  strace 1
    admin@MiniOS> sudo trace off
    admin@MiniOS> sudo schedtrace on         then: spawn demo 2   and   schedtrace
    admin@MiniOS> sudo logsave               writes /var/log/system.log
    admin@MiniOS> logstats                   capacity, levels, overwritten entries, trace state

Entries from the sources `sec` (logins, sudo, denied actions) and `shell` (which commands ran) are **private: only root
can read them**. Passwords never reach the log, and the shell logs command NAMES only, never arguments.
Full description: [docs/LOGGING.md](docs/LOGGING.md).

## New kernel pieces
- 3 system calls (62-64): `log_get log_ctl lock`, plus the PC speaker through `power` (operations 4 and 5). New error `EFLOCK` (-32).
- `kernel/log/logger.c`, `kernel/net/loopback.c`, `lock` support in `kernel/filesystem/filesystem.c`.
- Hooks: boot messages (`kernel.c`), audit events (`users.c`), process create/exit/fault (`process.c`, `workloads.c`),
  context switches (`scheduler.c`), kernel panics (`interrupts.c`), system calls (`syscall.c`), command names (`shell.c`).
- Changing the log (`loglevel`, `logclear`, `trace`, `schedtrace`) and `logsave` need root; reading is open to everybody.

## Default accounts (change with `passwd`)
root/root123, admin/admin123 (sudo), student/student123, guest/guest.

## Demo (presentation)
    login: admin / admin123
    admin@MiniOS> demo                      (guided tour; Enter for each step)
    admin@MiniOS> cpuinfo / irqinfo / date / devices / kernelinfo
    admin@MiniOS> echo hello > /tmp/a.txt / protect /tmp/a.txt / rm /tmp/a.txt   (refused)
    admin@MiniOS> sudo savefs / sudo rmtree /tmp / sudo loadfs --yes             (restored)
    admin@MiniOS> selftest / diag / calc 2*(3+4) / cal / ping gateway.local
    admin@MiniOS> sudo shutdown

## Honest limits
- Not run in QEMU by the author of this phase: it builds without warnings and every host suite passes, but the code that
  only exists in the kernel build (boot messages, the log hooks inside the timer interrupt and the scheduler, the PC speaker,
  the CMOS clock) was never executed on a machine. Please boot it and try `dmesg`, `logs`, `sudo trace on`, `sudo schedtrace on`,
  `loopback demo`, `beep`, `clock`.
- The log holds 256 entries in RAM and is lost at reboot unless `logsave` writes it to a file. `trace on` can fill it in seconds.
- Locks are not saved by `savefs` and disappear on `loadfs`; they apply to files, not directories.
- `loopback` messages are not private: anybody who may receive on a port sees what was sent to it.
- ONE console session and ONE identity for the whole kernel; no ring 3, no per-process credentials, no sticky bit.
- `diff` compares line N with line N (no alignment); `factor`/`prime` accept 32-bit values; `sort` handles 4096 lines.
