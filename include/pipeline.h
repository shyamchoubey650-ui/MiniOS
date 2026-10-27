#ifndef PIPELINE_H
#define PIPELINE_H
/* Pipelines (Phase 13): "cmd1 | cmd2 | cmd3 > file".
 *
 * HOW IT WORKS: MiniOS has no stdin/stdout streams, so a pipe is built from two things that already exist.
 *   - Output capture (the one behind '>'): the shell runs cmd1 with its screen output going into a 32 KiB buffer.
 *   - The file name "-": cmd2 reads that buffer by loading the file "-" (cu_load_file). For the commands listed in
 *     shell/pipeline.c the shell adds the "-" itself ("sort -r" becomes "sort -r -"); any other command can be given
 *     "-" by hand ("diff other.txt -").
 * The stages run one after another (not at the same time) and the whole pipeline is one shell command.
 *
 * RULES: at most 4 stages; only the last stage may use > or >>; each stage's output is limited to 32 KiB;
 *        if a stage FAILS (it printed "Error:" or "usage:") its message is shown and the pipeline stops, so an error
 *        message never becomes data for the next command; a stage that merely finds nothing (grep with no match)
 *        is not a failure. sudo, su, login, logout, passwd and exit cannot be part of a pipeline (their prompts
 *        would be captured). The pipeline's exit status is the status of its last command. */
#include "include/shell.h"
#include "include/command.h"

#define PIPE_BUFFER (32u * 1024u)

typedef const command_t *(*pipe_lookup_fn)(const char *name);

/* Runs a parsed pipeline (one stage = an ordinary command with optional redirection). `lookup` finds a command by name
 * (it may know legacy aliases); `is_owner` tells the capture whose output to take (NULL = everybody's). Returns the exit status. */
int  pipeline_run(const pipeline_t *p, pipe_lookup_fn lookup, bool (*is_owner)(void));

/* How the shell feeds a pipe to a command: PIPE_NO = the command cannot read a pipe. */
typedef enum { PIPE_NO, PIPE_DASH, PIPE_F_DASH } pipe_input_t;
pipe_input_t pipeline_input_kind(const char *command_name);   /* PIPE_DASH: append "-"; PIPE_F_DASH: append "-f" "-" */
bool         pipeline_forbidden(const char *command_name);    /* sudo, su, login, logout, passwd, exit */
#endif
