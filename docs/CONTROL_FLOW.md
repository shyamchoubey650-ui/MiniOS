# Control flow (Phase 15)

`if / elif / else / fi`, `for / while ... do ... done`, `break`, `continue` - implemented in `shell/parser.c` (the keyword layer: `expand_keywords`,
`command_list_incomplete`), `shell/lineexec.c` (the executor), `commands/shellenv_commands.c` (`source` joins the lines of a block) and
`shell/shell.c` (the `> ` continuation prompt and Ctrl+C). Tests: `tests/test_shell.c` (164 checks with the earlier parser tests) and
`tests/test_phase15.c` (142 end-to-end checks through the real commands, MiniFS, system calls and kernel log).

## Grammar
    list      := command { ( ';' | '&&' | '||' ) command } [ ';' ]
    command   := pipeline | if-block | for-block | while-block | 'break' | 'continue'
    if-block  := 'if' list ';' 'then' list { ';' 'elif' list ';' 'then' list } [ ';' 'else' list ] ';' 'fi'
    for-block := 'for' NAME 'in' WORD... ';' 'do' list ';' 'done'
    while-block := 'while' list ';' 'do' list ';' 'done'
A keyword is a keyword only as the FIRST word of a command (after `;`, `&&`, `||`, or another keyword): `echo done` is an ordinary command and `'if'` in
quotes is text. Keywords never take text after them except `if elif while then else do` (which are followed by a command) and `for` (its header);
`fi echo`, `done | sort`, `break now` are errors.

## How it runs
1. `split_command_list` cuts the line at `;`, `&&`, `||` (outside quotes) - Phase 14.
2. `expand_keywords` makes every keyword an entry of its own ("then echo y" becomes THEN, echo y).
3. A **dry run** walks the whole structure once: matching if/fi and loop/done, then/elif/else order, empty blocks, `break` outside a loop, nesting depth.
   Any problem is printed as `Error: syntax error: ...` with status 2, and **nothing has run**.
4. The real run executes left to right. Each simple command is alias-expanded, `$variable`-expanded and parsed on its own just before it runs, so
   `for` bodies see the new loop variable on every turn and `set` inside a body affects the next command.
Operators keep their Phase 14 meaning; a skipped block is skipped as a whole and leaves the status as it was.

## Status values
| Construct | Status |
|---|---|
| `if` | status of the last command of the branch that ran; 0 if no branch ran |
| `for`, `while` | status of the last turn's body; 0 if it never ran |
| `break`, `continue` | 0 (they succeed) |
| syntax error | 2 |
| loop stopped by the 1000-turn limit or the 5000-command limit | 1 |
| stopped by Ctrl+C | 130 |

## for
`for VAR in words ; do ... ; done`: the word list is variable-expanded (`$L` with `L='x y z'` gives three words), then split like a command line, so quotes
group words (`"a b"` is one). `>` and `|` are not allowed in it. At most 28 words of at most 63 characters. VAR must be a valid name; it keeps its last value
afterwards. An empty list means no turns.

## Several lines
- **Interactive:** when a line leaves an if/for/while open, the shell does not run it. It shows `> ` and reads another line; when the block is complete the lines are
  joined with ` ; ` and run as one statement (and recorded as one history entry). Ctrl+C cancels the block. A block longer than 500 characters is discarded.
- **Scripts:** `source file` joins the lines of a block the same way (limit 511 characters per statement). Blank lines and `#` comments are skipped, also inside blocks.
  If the file ends inside a block it reports the line where the block began and runs nothing from it.

## Safety limits
| Limit | Value | When reached |
|---|---|---|
| turns of one loop | 1000 | `Error: the loop was stopped after 1000 turns`, status 1 |
| commands run by one line | 5000 | `Error: this line ran more than 5000 commands and was stopped`, status 1 |
| Ctrl+C | polled once per loop turn | `^C`, status 130 (other keys typed meanwhile are discarded) |
| nesting depth | 8 | in practice the 16-commands-per-line limit is reached first (seven levels fit) |
| commands per line | 16 (keywords count) | `a command list can have at most 16 commands` |

## Memory and stack
The per-line state (about 2 KB) and the `for` word list (about 2 KB) are allocated on the kernel heap and freed on every exit path, because the shell runs on a
16 KiB stack and `source` can nest four levels. The tests run 240 blocks in a row and check that the free heap is unchanged.

## Security notes
Control flow changes no permissions: every command still runs as the logged-in user through the system calls (a student's `for f in secret ; do cat $f ; done` is refused).
`sudo` may appear in a loop body (only pipelines forbid it). The kernel log (DEBUG, private source `shell`) records only the names of commands that really ran.
`$USER` is an ordinary variable that a loop may overwrite; the prompt and all permission checks use the real credentials, not the variable.

## Not supported
Piping or redirecting a whole block (`done | sort`, `fi > f`), `$(...)`, arithmetic (use `calc`), `case`, functions, parentheses, `until`, `select`.
