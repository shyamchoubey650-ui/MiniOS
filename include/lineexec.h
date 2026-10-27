#ifndef LINEEXEC_H
#define LINEEXEC_H
/* One shell input line: command lists (Phase 14) and control flow (Phase 15) around pipelines (Phase 13).
 *
 *   a ; b      run b after a, whatever happened          a && b   run b only if a succeeded (status 0)
 *   a || b     run b only if a failed (status not 0)     a | b > f   a pipeline is ONE command
 *
 *   if C ; then A ; elif C2 ; then B ; else D ; fi
 *   for VAR in w1 w2 w3 ; do BODY ; done
 *   while C ; do BODY ; done              break and continue work inside for and while
 *
 * C is any command list (it may use &&, || and pipelines); it is true when its status is 0. A keyword is recognised only as the FIRST
 * word of a command, so "echo if" is ordinary text. Operators are read left to right with equal priority; a skipped command leaves
 * the status as it was. Every command is alias-expanded, $variable-expanded and parsed ON ITS OWN just before it runs.
 * The status of a line is the status of the last command that ran (0 for an if that took no branch or a loop that never ran).
 *
 * The whole line is checked BEFORE anything runs: a missing fi/done, a stray then/do, an empty block or a break outside a loop is
 * reported as a syntax error (status 2) and nothing is executed.
 *
 * Safety limits: a loop stops after 1000 turns, one line may run at most 5000 commands, Ctrl+C stops a loop (status 130).
 * Not supported: piping or redirecting a whole block ("done | sort"), parentheses, $(...), arithmetic (use calc), functions. */
#include "include/pipeline.h"

#define LINE_MAX_LOOP_TURNS 1000
#define LINE_MAX_COMMANDS   5000

/* Runs `raw`. *ran is false for a blank line (nothing happened). Returns the line's exit status. */
int  line_execute(const char *raw, pipe_lookup_fn lookup, bool (*is_owner)(void), bool *ran);
/* The shell sets a function that returns true when the user pressed Ctrl+C; loops poll it once per turn. May be NULL. */
void line_set_interrupt_check(bool (*pressed)(void));
#endif
