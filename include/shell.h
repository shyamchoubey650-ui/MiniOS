#ifndef SHELL_H
#define SHELL_H
#include "include/types.h"

#define SHELL_LINE_MAX   128   /* max characters in one input line   */
#define SHELL_MAX_ARGS   16    /* max words (argv) per command       */
#define SHELL_MAX_TOKENS 32    /* words + redirection and pipe operators */
#define PIPE_MAX_STAGES  4     /* commands in one pipeline (a | b | c | d) */
#define HISTORY_SIZE     16    /* remembered command lines           */

/* ---- tokenizer (shell/tokenizer.c) ---- */
typedef enum { TOK_WORD, TOK_REDIR_OUT, TOK_REDIR_APPEND, TOK_PIPE } token_type_t;
typedef struct { token_type_t type; const char *text; } token_t;

#define TOK_ERR_UNTERMINATED (-1)  /* quote opened but never closed */
#define TOK_ERR_TOO_MANY     (-2)  /* more tokens than the array holds */
#define TOK_ERR_BUFFER       (-3)  /* scratch buffer too small */

/* Splits `line` into tokens. Strings are copied into `buf` (NUL-terminated),
 * `out[i].text` points into `buf`. Supports "double" and 'single' quotes;
 * '>', '>>' and '|' outside quotes are operators. Returns token count or TOK_ERR_*. */
int tokenize(const char *line, char *buf, size_t bufsz, token_t *out, int max);

/* ---- parser (shell/parser.c) ---- */
typedef enum { REDIR_NONE, REDIR_TRUNCATE, REDIR_APPEND } redir_mode_t;

typedef struct {
    int          argc;
    const char  *argv[SHELL_MAX_ARGS + 1];  /* argv[argc] == NULL */
    redir_mode_t redir;
    const char  *redir_path;
} command_line_t;

#define PARSE_OK              0
#define PARSE_EMPTY           1   /* blank line: nothing to run */
#define PARSE_ERR_QUOTE      (-1)
#define PARSE_ERR_TOO_MANY   (-2)
#define PARSE_ERR_BUFFER     (-3)
#define PARSE_ERR_REDIR_TARGET (-4) /* '>' with no file name */
#define PARSE_ERR_REDIR_TWICE  (-5)
#define PARSE_ERR_NO_COMMAND   (-6) /* '> file' with no command */
#define PARSE_ERR_EMPTY_STAGE  (-7) /* '|' with no command next to it: "a |", "| b", "a | | b" */
#define PARSE_ERR_PIPE_REDIR   (-8) /* '>' on a command that is not the last of its pipeline */
#define PARSE_ERR_TOO_MANY_STAGES (-9)
#define PARSE_ERR_PIPE         (-10) /* parse_command_line (one command) was given a pipeline */
#define PARSE_ERR_LIST_EMPTY   (-11) /* ';' '&&' '||' with no command on one side ("a ;; b", "&& a", "a &&") */
#define PARSE_ERR_BACKGROUND   (-12) /* a lone '&' */
#define PARSE_ERR_LIST_TOO_LONG (-13)
#define PARSE_ERR_KW_TEXT      (-14) /* text after fi, done, break or continue: "fi echo", "done | sort" */

int         parse_command_line(const char *line, char *buf, size_t bufsz, command_line_t *out);   /* ONE command; PARSE_ERR_PIPE if the line has a '|' */

/* A pipeline: up to PIPE_MAX_STAGES commands joined by '|'. Only the LAST one may redirect its output with > or >>.
 * A line without '|' is a pipeline of one stage. All argv strings point into `buf`. */
typedef struct { int nstages; command_line_t stage[PIPE_MAX_STAGES]; } pipeline_t;
int         parse_pipeline(const char *line, char *buf, size_t bufsz, pipeline_t *out);
const char *parse_error_string(int code);

/* ---- command lists (shell/parser.c): "a ; b", "a && b", "a || b" ----
 * A line is cut at ';', '&&' and '||' that are outside quotes into SEGMENTS. Each segment is then expanded (aliases, $variables)
 * and parsed as a pipeline ON ITS OWN, just before it runs, so "set X=1 ; echo $X" and "false ; echo $?" see the new values. */
#define LIST_MAX_SEGMENTS 16
typedef enum { CONN_FIRST, CONN_SEMI, CONN_AND, CONN_OR } connector_t;   /* how a segment is tied to the one BEFORE it */
typedef struct { const char *text; size_t len; connector_t conn; } segment_t;
/* Returns the number of segments (0 for a blank line) or PARSE_ERR_*. A trailing ';' is allowed ("echo hi ;"). */
int         split_command_list(const char *line, segment_t *seg, int max);

/* ---- control flow keywords (Phase 15) ----
 * if / elif / else / fi, while / for / do / done, break, continue. A keyword counts only as the FIRST word of a segment, so
 * "echo if" is an ordinary command and "if" inside quotes is text. expand_keywords turns the segments of a line into a flat
 * list in which every keyword is a segment of its own: "if a ; then b ; fi" becomes IF, a, THEN, b, FI. */
typedef enum { KW_NONE, KW_IF, KW_THEN, KW_ELIF, KW_ELSE, KW_FI, KW_WHILE, KW_FOR, KW_DO, KW_DONE, KW_BREAK, KW_CONTINUE } keyword_t;
typedef struct { const char *text; size_t len; connector_t conn; keyword_t kw; } xseg_t;   /* text = the command; for KW_FOR the header "VAR in words..."; empty for other keywords */
#define XSEG_MAX 40
int         expand_keywords(const segment_t *raw, int n, xseg_t *out, int max);   /* count, or PARSE_ERR_* */
bool        command_list_incomplete(const char *line);   /* true if the line leaves an if/while/for open: a script or the prompt should read another line */

/* ---- history (shell/history.c) ---- */
void        history_init(void);
void        history_add(const char *line);   /* ignores blank / repeated-last lines */
int         history_count(void);
const char *history_get(int index);          /* 0 = oldest; NULL if out of range */

/* ---- shell main loop (shell/shell.c) ---- */
void shell_run(void);
int  shell_last_exit(void);   /* exit status of the previous command */
#endif
