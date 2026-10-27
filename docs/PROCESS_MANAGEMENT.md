# MiniOS process management (Phase 7)

## What is REAL and what is not

| Feature | Status | Notes |
|---|---|---|
| Process Control Block, process table (16 slots) | **REAL** | `include/process.h`, `kernel/process/process.c` |
| Separate kernel stack per process (8 KiB from the heap) | **REAL** | freed when the process dies |
| Context switching (callee-saved registers + stack pointer) | **REAL** | `boot/context.S`, tested natively in 32-bit user space |
| Preemption by the timer interrupt | **REAL** | IRQ0 -> `sched_tick` -> switch at interrupt exit |
| Round-robin scheduler, configurable time slice | **REAL** | `kernel/process/scheduler.c`, 1-100 ticks (10 ms each) |
| States NEW / READY / RUNNING / WAITING / TERMINATED | **REAL** | zombies keep their exit code until `wait` |
| sleep, suspend/resume, wait for child, Ctrl+C aborts wait | **REAL** | |
| A faulting process is killed instead of crashing the OS | **REAL** | divide-by-zero etc. in PID >= 2 only |
| Stack-overflow detection (canary word) | **REAL, best effort** | checked each time a process is switched out |
| Priority scheduling | **NOT IMPLEMENTED** | priority is stored and shown, the scheduler ignores it |
| User mode, separate address spaces, memory protection | **NOT IMPLEMENTED** | every process is a ring-0 kernel task sharing memory |
| Loading programs from the file system | **NOT IMPLEMENTED** | `spawn` starts built-in demo functions (SIMULATED programs, REAL tasks) |
| `bg`, `fg` | **REAL (shell children only)** | `bg` resumes a suspended child, `fg` resumes and waits (Ctrl+C stops waiting). No terminal job control, no Ctrl+Z |
| `schedtrace` | **REAL** | `sudo schedtrace on` logs every context switch to the kernel log ([LOGGING.md](LOGGING.md)) |
| `memlimit` | **ACCOUNTING ONLY** | records a limit per process and reports `OVER`; nothing is stopped |

## Process model

* **PID 0 `idle`**: runs `hlt` when nothing else is READY; never in the ready queue; hidden from `ps` unless `ps -a`.
* **PID 1 `shell`**: the code that has been running since boot, adopted as a process. It runs on the boot stack, cannot be killed or suspended.
* **PID >= 2**: created by `spawn`/`nice`/`procdemo`. Parent = the creator. When a parent dies its children are adopted by the shell.

```
        create                 dispatch                 slice expires / yield
 NEW ----------> READY <------------------ RUNNING -----------------------> READY
                   ^                          | exit / kill                    
                   | wake (sleep ends,        v                                
                   | child exits, resume,  TERMINATED (zombie) --wait--> slot free
                   | Ctrl+C)               
                 WAITING <------ sleep / wait(child) / suspend ------ RUNNING
```

PCB (`PCB` in `process.h`): `pid`, `name[32]`, `state`, `priority`, `memory_used`, `parent_pid` as specified, plus `exit_code`, wait bookkeeping, `cpu_ticks`, `times_scheduled`, saved `esp`, stack pointer/size, entry function.

## Scheduler

* FIFO ready queue; the running process owns the CPU for one **time slice** (default 5 ticks = 50 ms, `setquantum 1-100`).
* The timer tick (100 Hz) charges the running process (`cpu_ticks`), wakes sleepers, and when the slice reaches 0 *and another process is READY* sets a reschedule flag. If nobody else is READY the slice simply refills.
* The switch happens at the **end of the interrupt**, after the PIC was acknowledged (EOI is now sent *before* the handlers run, otherwise the next timer tick would never arrive after a switch).
* Voluntary switches (yield, sleep, wait, suspend, exit) call `schedule()` directly.
* Counters: `ctxstat` shows total switches, preemptions, voluntary switches, idle ticks.

## How a context switch works (`boot/context.S`)

```
push ebp, ebx, esi, edi          ; callee-saved registers go on the OLD stack
mov  [old_esp], esp              ; remember where the old task stopped
mov  esp, new_esp                ; ---- now on the other task's stack ----
pop  edi, esi, ebx, ebp
ret                              ; returns into the other task
```
A new task's stack is pre-built (`arch_stack_init`) with zeroed registers and the address of `process_trampoline` as the return target, so the first switch "returns" into it. The trampoline frees a dead predecessor's stack, enables interrupts (it was entered from inside an interrupt) and calls the task function; when that returns the process exits with code 0.

Safety rules used:
* A running task's stack is never freed by itself: the *next* task frees it (`sched_finish_switch`).
* The screen driver and heap disable interrupts around their critical sections so a preemption cannot corrupt the cursor or the allocator. Interleaved *lines* from several processes are still possible.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | normal exit |
| 137 | killed (`kill`, `pkill`, `killall`) |
| 139 | stack overflow detected |
| 128 + n | CPU exception n (128 = divide error, 134 = invalid opcode, 141 = general protection fault) |

Codes 137 and 139 coincide with exceptions 9 and 11, which do not occur in practice here.

## Commands (26, all in `commands/process_commands.c`)

`ps spawn kill wait top pstree pid ppid nice renice priority suspend resume sched quantum setquantum yield jobs pinfo pkill killall pgrep runqueue ctxstat proccount procdemo`

`spawn` programs: `demo [s]` (CPU for s seconds), `spin` (forever), `sleeper [s]` (WAITING), `ticker [n]` (prints once per second), `crash` (divides by zero).

## Testing

| File | What it checks |
|---|---|
| `tests/test_process.c` (108) | create/limits/memory exhaustion, kill (self, sleeping, protected, double), wait/exit codes, orphans, Ctrl+C, suspend/resume, priority bounds, sleep timing, stack canary, round-robin fairness (exactly equal CPU ticks), time-slice changes, idle task behaviour |
| `tests/test_process_commands.c` (97) | every process command via the real parser: valid input, bad PIDs, protected processes, full table, crash containment |
| `tests/native_context.c` | the real `context.S`: 200000 switches, callee-saved registers preserved, stack alignment, correct task order |

The scheduler tests replace only the final register swap with a stub; a `setjmp` trick simulates "this call blocked and another task took over". Deliberately breaking the code (no parent wake-up, no dequeue on kill, no stack free, slices never expiring, wrong sleep timing, no canary check, ...) makes these tests fail.

At boot the kernel also runs a **scheduler self-test**: it starts a real task, lets the timer preempt the shell into it, and checks that it ran on its own stack, exited, and that the heap is back to where it started.

## Limitations

* Single CPU, no locking beyond disabling interrupts.
* A runaway process can still overwrite kernel memory (no protection) and a `spin` process shares the CPU with the shell, so the shell stays responsive only because of preemption.
* Time slice granularity is 10 ms; priorities are cosmetic.
* Output from background tasks (`ticker`) can appear in the middle of what you are typing.
