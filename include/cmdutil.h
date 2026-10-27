#ifndef CMDUTIL_H
#define CMDUTIL_H
/* Small helpers shared by the Phase 8 command files. */
#include "include/types.h"

#define CU_ANY  0
#define CU_FILE 1
#define CU_DIR  2

void cu_color(uint8_t fg);
int  cu_usage(const char *text);                                   /* prints "usage: ..." and returns 1 */
int  cu_error(const char *cmd, const char *msg);                   /* "Error: cmd: msg", returns 1 */
int  cu_fs_error(const char *cmd, const char *path, int fs_rc, int kind);   /* maps an FS_ERR_* code to a message, returns 1 */
void cu_pad(const char *s, size_t width);
void cu_num_pad(uint32_t v, size_t width);
void cu_size_str(char *buf, uint32_t bytes);                       /* "4.0 MiB" (buf >= 16 bytes) */
void cu_put_size(uint32_t bytes);
void cu_print_text(const char *buf, size_t len);                   /* control characters shown as '.' */
bool cu_parse_count(const char *s, uint32_t *out, uint32_t max);  /* 0..max, decimal or 0x */

/* Reads a whole file into a fresh kmalloc buffer (NUL-terminated, *len excludes the NUL).
 * On failure prints the error and returns false. Caller frees with cu_free_file(). Uses sys_stat / sys_open / sys_read /
 * sys_close / sys_memory_alloc only. */
bool cu_load_file(const char *cmd, const char *path, char **buf, size_t *len);   /* the path "-" returns the pipe input (see below) */

/* Pipe input (Phase 13). In "a | b" the shell makes a's output available to b as the file name "-". Every command that loads
 * its input with cu_load_file therefore reads a pipe for free. The text is only valid while that one stage runs. */
void cu_pipe_set(const char *buf, size_t len);
void cu_pipe_clear(void);
bool cu_pipe_active(void);
void cu_free_file(char *buf);
bool cu_read_secret(const char *prompt, char *buf, size_t cap);    /* reads a line from stdin, echoing '*'; false on Ctrl+C / too long */
void cu_sleep_ms(uint32_t ms);                                     /* sleeps through the scheduler (no-op in host tests) */
void cu_auth_delay(void);                                          /* one-second pause after a failed authentication (slows guessing; no-op in host tests) */
void cu_put_u64(uint64_t v);                                       /* decimal, no libgcc needed */
void cu_put_i64(int64_t v);
void cu_wipe(void *p, size_t n);                                   /* zeroes memory in a way the compiler cannot remove */
void cu_uid_name(uint32_t uid, char *out, size_t cap);             /* "student", or the number if no such account */          /* releases a buffer from cu_load_file (a system-call allocation) */
#endif
