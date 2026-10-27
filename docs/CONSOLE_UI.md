# Console UI (Phase 16)

Scrollback, Tab completion, in-line editing and the folder in the prompt. The logic is in three pure modules that are unit-tested on the host; the screen, keyboard
and shell are thin wiring around them.

| Piece | File | Tests |
|---|---|---|
| edit buffer: insert, delete, cursor moves, Ctrl+K/U/W, history replace, completion replace, prompt path | `lib/lineedit.c` | `tests/test_lineedit.c` |
| scrollback ring and "which rows to draw" | `lib/scrollback.c` | `tests/test_lineedit.c` |
| completion engine (commands, variables, files) | `lib/complete.c` | `tests/test_lineedit.c` (fake lists) and `tests/test_phase16.c` (real command table, variables, MiniFS) |
| real lists for completion | `shell/compenv.c` | `tests/test_phase16.c` |
| keys Home, End, PageUp, PageDown | `kernel/device/keyboard.c` | wiring only |
| VGA scrollback view, cursor placement | `kernel/device/screen.c` | wiring only |
| key handling, redraw, Tab, prompt | `shell/shell.c` | wiring only |

## Keys
Left, Right, Home, End, Delete, Backspace, Ctrl+A (start), Ctrl+E (end), Ctrl+K (cut to end), Ctrl+U (cut line), Ctrl+W (cut word), Ctrl+L (clear, the cleared text stays in
scrollback), Ctrl+C (cancel the line or an unfinished block), Up/Down (history), Tab (complete), PageUp/PageDown (scrollback, 20 lines per press).

## Editing and redrawing
The text and the cursor live in an `edit_t` (127 characters). Typing at the end of the line or Backspace at the end print or erase one character as before. Any other change
(typing in the middle, Delete, Ctrl+K/U/W, history, completion) redraws the line from where the input started and puts the cursor back. The shell remembers the screen row and
column where the input begins and the screen's scroll counter, so a line that wraps and scrolls the screen is still redrawn from the right place.

## Scrollback
Every line that scrolls off the top is copied into a ring of 200 lines (`SB_LINES`, about 32 KB of kernel memory). Clearing the screen also saves the text that was on it.
PageUp copies the live screen aside, hides the cursor and draws a window onto "saved lines + live screen"; a blue bar on the top row names the keys. PageDown comes back; reaching the
bottom, pressing any other key, or any output restores the live screen exactly. The boot loader's text is not kept.

## Tab completion
`complete_word(line, cursor, env, result)` finds the word under the cursor and decides what it is:
1. starts with `$`: a variable name (`${NA` completes to `${NAME}`)
2. at command position (start of the line, after `;`, `|`, `&`, or after `then do else if elif while sudo`) and without a `/`: a command name or a shell keyword
3. anything else: a file or directory name; the part up to the last `/` is the directory (empty = the working directory)
Result kinds: `NONE`, `UNIQUE` (complete the whole word; directories get `/`, files a space), `PARTIAL` (the names share a longer start: insert it) and `LIST` (nothing more to add:
the shell prints the names in columns and redraws the prompt and the line). At most 64 names are kept and shown; the result says how many there were in total.
Names are offered only if they are safe to type: no space, quote, `; | & < > $ \ ( ) * ? ``. Hidden names need a typed dot. Directory names come from `sys_readdir`, so permissions apply:
a student gets nothing for a directory he cannot read.

## Prompt
`user@MiniOS:path>`: the user name is green (root: red), the path is cyan. The home directory (from the user table) is shown as `~`; a path longer than 28 characters keeps its last
two parts (`.../package/files`). If the variable `PS1` is set, it is used instead, as before.

## Limits
200 scrollback lines; one 80-column screen; 127-character lines; completion of one word at a time, not inside quotes; no mouse. Keys typed while a long command or loop runs are not
processed until it ends (except Ctrl+C, which loops poll).
