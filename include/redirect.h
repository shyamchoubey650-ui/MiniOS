#ifndef REDIRECT_H
#define REDIRECT_H
#include "include/types.h"
#include "include/shell.h"
#include "include/filesystem.h"

#define REDIRECT_BUFFER (32u * 1024u)       /* max output of one redirected command */

typedef struct {
    char        *buf;
    redir_mode_t mode;
    char         path[FS_PATH_MAX];
    bool         active;
} redirect_t;

/* Validates the target (parent exists, not a directory, writable) BEFORE the command runs,
 * then starts capturing. Returns FS_OK or an FS_ERR_* code (nothing is started on error). */
int redirect_begin(redirect_t *r, const char *path, redir_mode_t mode, bool (*is_owner)(void));

/* Stops capturing. If the command succeeded (status 0) the captured text is written to the file;
 * if it failed, the captured text (its error message) is shown on screen and the file is left
 * untouched. Returns the status the shell should record (1 if writing the file failed). */
int redirect_end(redirect_t *r, int status);
#endif
