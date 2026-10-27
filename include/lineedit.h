#ifndef LINEEDIT_H
#define LINEEDIT_H
/* The editable input line (Phase 16): a buffer, a cursor, and the operations behind Left, Right, Home, End, Delete, Backspace,
 * Ctrl+A/E/U/K/W and Tab completion. Pure logic: the shell only turns key codes into these calls and redraws the line.
 * Every function returns true if the text or the cursor changed (so the caller knows whether to redraw). */
#include "include/types.h"

#define EDIT_MAX 128                       /* characters including the terminating NUL (= SHELL_LINE_MAX) */
typedef struct { char buf[EDIT_MAX]; size_t len, pos; } edit_t;

void edit_init(edit_t *e);
bool edit_insert(edit_t *e, char c);       /* at the cursor; false if the line is full or c is not printable */
bool edit_backspace(edit_t *e);            /* the character before the cursor */
bool edit_delete(edit_t *e);               /* the character under the cursor */
bool edit_left(edit_t *e);
bool edit_right(edit_t *e);
bool edit_home(edit_t *e);
bool edit_end(edit_t *e);
bool edit_kill_to_end(edit_t *e);          /* Ctrl+K */
bool edit_kill_line(edit_t *e);            /* Ctrl+U: the whole line */
bool edit_kill_word(edit_t *e);            /* Ctrl+W: the word before the cursor and the spaces before it */
bool edit_set(edit_t *e, const char *text);                              /* history: replace everything, cursor at the end; text is cut to fit */
bool edit_replace(edit_t *e, size_t from, size_t to, const char *text);  /* completion: replace buf[from..to) and put the cursor after the new text; false if it does not fit */

/* A readable name for a key code from the keyboard driver: "PageUp", "Ctrl+C", "Enter", "a" ... (kbdinfo uses it). Returns the length. */
size_t key_name(int code, char *out, size_t cap);

/* The directory shown in the prompt: the home directory becomes "~" ("~/docs"), and a long path keeps its last two parts (".../a/b"). */
size_t prompt_path(const char *cwd, const char *home, char *out, size_t cap);
#endif
