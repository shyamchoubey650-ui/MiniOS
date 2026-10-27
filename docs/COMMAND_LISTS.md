# Command lists (Phase 14)

`a ; b`, `a && b`, `a || b` - implemented in `shell/parser.c` (`split_command_list`), `shell/lineexec.c` (`line_execute`) and used by
`shell/shell.c`. Tests: `tests/test_shell.c` (the splitter, 135 checks with the pipe parser) and `tests/test_phase14.c` (79 end-to-end checks).

## Grammar
    line     := command { ( ';' | '&&' | '||' ) command } [ ';' ]
    command  := pipeline  (Phase 13:  stage { '|' stage } [ '>' file | '>>' file ])
At most 16 commands per line (since Phase 15; control-flow keywords count as commands). `;`, `&&` and `||` are operators only OUTSIDE quotes. A single `|` is a pipe, two are OR. A single `&` is an error.

## Meaning
| Operator | The next command runs when |
|---|---|
| `;` | always |
| `&&` | the previous status is 0 |
| `||` | the previous status is not 0 |
Operators have equal priority and are applied left to right. A skipped command does not change the status. `$?` and the exit status of
the whole line are the status of the last command that really ran. A command name that does not exist has status 127.

## Order of work
1. `split_command_list` cuts the raw line into segments (quote-aware). Syntax errors stop everything here: nothing has run yet.
2. For each segment, in order: decide whether to skip it, then expand aliases and `$variables`, parse it as a pipeline, run it, set `$?`.
Because expansion happens per segment, `set X=1 ; echo $X` prints 1 and `false ; echo $?` prints 1.

## Errors
| Input | Result |
|---|---|
| `;` `a ;; b` `&& a` `a &&` `|| a` `a ||` | missing command next to `;`, `&&` or `||` (status 2, nothing runs) |
| `a & b`, `a &` | background jobs are not supported: use `spawn` |
| more than 16 commands | a command list can have at most 16 commands |
| `echo "abc` | unterminated quote (found before anything runs) |
| `echo ok ; echo hi >` | `echo ok` runs; the second command fails with "missing file name" (status 2) |

## Security notes
Lists change no permissions: each command still runs as the logged-in user through the system calls. `sudo` is allowed in a list (only
pipelines forbid it). The kernel log (DEBUG, private source `shell`) records only the NAMES of commands that really ran, never skipped
ones and never arguments.

## Limits
16 commands, no parentheses, no `$(...)`, no background jobs. Control flow: see [CONTROL_FLOW.md](CONTROL_FLOW.md).
