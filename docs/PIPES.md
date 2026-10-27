# Pipes (Phase 13)

`cmd1 | cmd2 | cmd3 > file` - implemented in `shell/pipeline.c` (runner), `shell/parser.c` + `shell/tokenizer.c` (syntax) and
`commands/cmd_util.c` (the input side). Tests: `tests/test_shell.c` (94 parser checks) and `tests/test_phase13.c` (237 end-to-end checks
through the real commands, MiniFS, system calls and output capture).

## The idea
MiniOS has no stdin and stdout streams: commands print straight to the screen and read files by name. A pipe is therefore built from two
parts that already existed:

1. **Output capture** - the mechanism behind `>`. While a stage runs, everything it prints goes into a 32 KiB buffer instead of the screen.
2. **The file name `-`** - `cu_load_file("-")` returns that buffer. All text commands load their input through `cu_load_file`, so they read a
   pipe without any change. The shell appends the `-` itself for the commands in the table below.

Two buffers take turns, so stage *n* reads buffer A while its own output goes into buffer B.

## Syntax
| Input | Meaning |
|---|---|
| `a | b | c` | up to 4 commands |
| `a | b > f`, `a | b >> f` | redirection of the LAST command's output |
| `a > f | b` | error: only the last command may redirect |
| `a |`, `| b`, `a || b` | error: missing command next to `|` |
| `echo "a | b"` | the pipe is inside quotes: ordinary text |

## How the shell feeds a command
| Kind | Commands | What the shell does |
|---|---|---|
| file operand | cat head tail wc grep sort uniq cut tr rev nl tac fold join wordfreq hexdump more | appends `-` (`sort -r` becomes `sort -r -`) |
| `-f` operand | upper lower capitalize sum min max avg | appends `-f -` |
| by hand | diff cmp paste (and any command with a file operand) | you write the `-`: `cat a | diff b -` |
| none | everything else (`ls`, `pwd`, `echo`, `contains`, `replace`, ...) | error: "does not read from a pipe" |

If you wrote a `-` yourself, nothing is added.

## What is checked before the first stage runs
unknown command names, commands that cannot read a pipe, forbidden commands, too many arguments, and whether the `>` target is writable.
Nothing has run when one of these errors is reported.

## Failure rules
- A stage **fails** if its exit status is not 0 AND its output starts with `Error:` or `usage:`. Its output is shown, `(pipeline stopped: 'x' failed)` is
  added, and the later stages do not run. This keeps error messages from becoming data (`cat missing | sort` would otherwise sort the message).
- A stage that exits with status 1 but printed normal output (`grep` finding nothing, `contains` printing `false`) is **not** a failure.
- A stage that prints more than 32 KiB stops the pipeline with an error. Nothing is truncated silently.
- The exit status of the line is the status of the last command that ran.

## Forbidden in a pipeline
`sudo su login logout passwd exit`: they read a password or end the session, and the capture would hide their prompts.
A pipeline also cannot start while another capture is active (for example inside `source script.sh | cat`).

## Security notes
- Each command still reads through system calls as the current user, so a pipe cannot read anything the user could not read.
- Private log entries stay private: `logs -a | grep victim` shows a normal user nothing.
- Only command NAMES are written to the kernel log (DEBUG, private source `shell`), never the arguments, for every stage.
- A protected or locked target of `>` is refused up front.

## Limits
4 stages, 32 KiB per stage, 16 arguments per command (the appended `-` counts), stages run one after another, no job control inside a pipeline.
