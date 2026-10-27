#include "include/pipeline.h"
#include "include/builtins.h"
#include "include/capture.h"
#include "include/redirect.h"
#include "include/cmdutil.h"
#include "include/screen.h"
#include "include/memory.h"
#include "include/logger.h"
#include "include/string.h"

/* Commands that read a FILE as their last operand: "-" is appended. */
static const char *const dash_cmds[] = {
    "cat", "head", "tail", "wc", "grep", "hexdump", "more",
    "sort", "uniq", "cut", "tr", "rev", "nl", "tac", "fold", "join", "wordfreq",
};
/* Commands that take "-f <file>": "-f" "-" is appended. */
static const char *const fdash_cmds[] = { "upper", "lower", "capitalize", "sum", "min", "max", "avg" };
/* Commands that ask for a password or end the session: their prompts would be swallowed by the capture. */
static const char *const forbidden_cmds[] = { "sudo", "su", "login", "logout", "passwd", "exit" };

static bool in_list(const char *const *list, size_t n, const char *name) { for (size_t i = 0; i < n; i++) if (!strcmp(list[i], name)) return true; return false; }
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

pipe_input_t pipeline_input_kind(const char *name) {
    if (in_list(dash_cmds, COUNT(dash_cmds), name)) return PIPE_DASH;
    if (in_list(fdash_cmds, COUNT(fdash_cmds), name)) return PIPE_F_DASH;
    return PIPE_NO;
}
bool pipeline_forbidden(const char *name) { return in_list(forbidden_cmds, COUNT(forbidden_cmds), name); }

static int invoke(const command_t *c, int argc, const char **argv) {
    int st = c->handler ? c->handler(argc, argv) : command_stub(c, argc, argv);
    klogf(LOG_DEBUG, "shell", "%s -> %d", c->name, st);          /* the NAME only: arguments may hold passwords */
    return st;
}

/* A stage "failed" if it said so in words. Exit status alone is not enough: grep returns 1 for "no match". */
static bool printed_error(const char *buf, size_t n) {
    return (n >= 6 && !memcmp(buf, "Error:", 6)) || (n >= 6 && !memcmp(buf, "usage:", 6));
}

static int error(const char *a, const char *b, const char *c, int status) {
    screen_puts("Error: "); screen_puts(a); if (b) screen_puts(b); if (c) screen_puts(c); screen_putc('\n');
    return status;
}

int pipeline_run(const pipeline_t *p, pipe_lookup_fn lookup, bool (*is_owner)(void)) {
    const command_t *cmd[PIPE_MAX_STAGES];
    const char *argv[PIPE_MAX_STAGES][SHELL_MAX_ARGS + 3];
    int argc[PIPE_MAX_STAGES];
    int n = p->nstages;

    /* 1. Check everything BEFORE running anything: a typo in the last command must not leave the first ones executed. */
    for (int i = 0; i < n; i++) {
        cmd[i] = lookup(p->stage[i].argv[0]);
        if (!cmd[i]) return error("unknown command '", p->stage[i].argv[0], "'. Type 'help'.", 127);
    }
    if (n > 1) {
        for (int i = 0; i < n; i++) if (pipeline_forbidden(cmd[i]->name))
            return error("'", cmd[i]->name, "' cannot be part of a pipeline (it asks for a password or ends the session). Run it on its own line.", 1);
        if (capture_active()) return error("a pipeline cannot run inside another redirection or pipeline.", NULL, NULL, 1);
    }
    for (int i = 0; i < n; i++) {
        argc[i] = p->stage[i].argc;
        for (int k = 0; k < argc[i]; k++) argv[i][k] = p->stage[i].argv[k];
        if (i == 0) continue;
        bool has_dash = false;
        for (int k = 1; k < argc[i]; k++) if (!strcmp(argv[i][k], "-")) has_dash = true;
        if (has_dash) continue;                                       /* the user placed the pipe input by hand */
        pipe_input_t kind = pipeline_input_kind(cmd[i]->name);
        if (kind == PIPE_NO)
            return error("'", cmd[i]->name, "' does not read from a pipe. Commands that do: cat head tail wc grep sort uniq cut tr rev nl tac fold join wordfreq hexdump more upper lower capitalize sum min max avg; or give it '-' as a file name.", 1);
        if (argc[i] + (kind == PIPE_F_DASH ? 2 : 1) > SHELL_MAX_ARGS) return error("too many arguments for a command in a pipeline.", NULL, NULL, 1);
        if (kind == PIPE_F_DASH) argv[i][argc[i]++] = "-f";
        argv[i][argc[i]++] = "-";
    }
    for (int i = 0; i < n; i++) argv[i][argc[i]] = NULL;
    const command_line_t *last = &p->stage[n - 1];
    if (last->redir != REDIR_NONE) {                                   /* the target must be usable before the first stage runs */
        int rc = fs_can_write(last->redir_path);
        if (rc != FS_OK) { screen_puts("Error: cannot redirect to '"); screen_puts(last->redir_path); screen_puts("': "); screen_puts(fs_strerror(rc)); screen_putc('\n'); return 1; }
    }

    /* 2. A single command (the usual case): exactly the old behaviour. */
    if (n == 1) {
        redirect_t rd; bool redirected = false;
        if (last->redir != REDIR_NONE) {
            int rrc = redirect_begin(&rd, last->redir_path, last->redir, is_owner);
            if (rrc != FS_OK) { screen_puts("Error: cannot redirect to '"); screen_puts(last->redir_path); screen_puts("': "); screen_puts(fs_strerror(rrc)); screen_putc('\n'); return 1; }
            redirected = true;
        }
        int st = invoke(cmd[0], argc[0], argv[0]);
        return redirected ? redirect_end(&rd, st) : st;
    }

    /* 3. A real pipeline: two buffers take turns. */
    char *buf[2] = { kmalloc(PIPE_BUFFER), kmalloc(PIPE_BUFFER) };
    if (!buf[0] || !buf[1]) { if (buf[0]) kfree(buf[0]); if (buf[1]) kfree(buf[1]); return error("out of memory for the pipe buffers.", NULL, NULL, 1); }
    int status = 0; size_t prev_len = 0;
    for (int i = 0; i < n; i++) {
        bool is_last = i == n - 1;
        if (i > 0) cu_pipe_set(buf[(i - 1) & 1], prev_len);
        redirect_t rd; bool redirected = false;
        if (!is_last) capture_begin(buf[i & 1], PIPE_BUFFER, is_owner);
        else if (last->redir != REDIR_NONE) {
            int rrc = redirect_begin(&rd, last->redir_path, last->redir, is_owner);
            if (rrc != FS_OK) { cu_pipe_clear(); screen_puts("Error: cannot redirect to '"); screen_puts(last->redir_path); screen_puts("': "); screen_puts(fs_strerror(rrc)); screen_putc('\n'); status = 1; break; }
            redirected = true;
        }
        status = invoke(cmd[i], argc[i], argv[i]);
        cu_pipe_clear();
        if (is_last) { if (redirected) status = redirect_end(&rd, status); break; }

        bool overflow = false;
        size_t got = capture_end(&overflow);
        if (overflow) {
            screen_puts("Error: the output of '"); screen_puts(cmd[i]->name); screen_puts("' is larger than 32 KiB; the pipeline was stopped\n");
            status = 1; break;
        }
        if (status != 0 && printed_error(buf[i & 1], got)) {            /* show the failure, do not feed it to the next command */
            for (size_t k = 0; k < got; k++) screen_putc(buf[i & 1][k]);
            screen_puts("(pipeline stopped: '"); screen_puts(cmd[i]->name); screen_puts("' failed)\n");
            break;
        }
        prev_len = got;
    }
    kfree(buf[0]); kfree(buf[1]);
    return status;
}
