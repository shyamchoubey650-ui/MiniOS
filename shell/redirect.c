#include "include/redirect.h"
#include "include/capture.h"
#include "include/memory.h"
#include "include/screen.h"

int redirect_begin(redirect_t *r, const char *path, redir_mode_t mode, bool (*is_owner)(void)) {
    if (!r || !path || mode == REDIR_NONE) return FS_ERR_INVALID;
    r->active = false;
    int rc = fs_can_write(path);
    if (rc != FS_OK) return rc;
    if (fs_resolve(path, r->path, sizeof r->path) != FS_OK) return FS_ERR_PATH_TOO_LONG;
    r->buf = kmalloc(REDIRECT_BUFFER);
    if (!r->buf) return FS_ERR_NO_SPACE;
    r->mode = mode; r->active = true;
    capture_begin(r->buf, REDIRECT_BUFFER, is_owner);
    return FS_OK;
}

int redirect_end(redirect_t *r, int status) {
    if (!r || !r->active) return status;
    bool overflow = false;
    size_t n = capture_end(&overflow);
    r->active = false;
    int result = status;

    if (status != 0) {                                   /* failed command: show what it printed, keep the file */
        for (size_t i = 0; i < n; i++) screen_putc(r->buf[i]);
    } else if (overflow) {
        screen_puts("Error: output is larger than 32 KiB; nothing was written to '"); screen_puts(r->path); screen_puts("'\n");
        result = 1;
    } else {
        int rc = fs_write(r->path, r->buf, n, r->mode == REDIR_APPEND ? FS_WRITE_APPEND : FS_WRITE_TRUNCATE);
        if (rc != FS_OK) {
            screen_puts("Error: cannot write '"); screen_puts(r->path); screen_puts("': "); screen_puts(fs_strerror(rc)); screen_putc('\n');
            result = 1;
        }
    }
    kfree(r->buf); r->buf = NULL;
    return result;
}
