/* MiniOS shell: line editor, history navigation and command dispatch.
 * Commands live in the central registry (commands/); this file only parses a
 * line, looks the command up and runs its handler (or the stub handler). */
#include "include/shell.h"
#include "include/pipeline.h"
#include "include/lineedit.h"
#include "include/complete.h"
#include "include/lineexec.h"
#include "include/logger.h"
#include "include/redirect.h"
#include "include/process.h"
#include "include/screen.h"
#include "include/keyboard.h"
#include "include/command.h"
#include "include/builtins.h"
#include "include/string.h"
#include "include/syscall.h"
#include "include/users.h"
#include "include/shellvars.h"
#include "include/builtins.h"

#define PROMPT_SUFFIX "@MiniOS> "

static int last_exit;
int shell_last_exit(void) { return last_exit; }

/* Phase 4 command names that are now spelled differently in the registry. */
static const struct { const char *alias, *target; } legacy_names[] = {
    { "int3", "breakpoint" },
    { "div0", "panic"      },
};

/* ---------- execution ---------- */
/* Only the shell's own output is captured by a redirection; background tasks still print normally. */
static bool shell_owns_output(void) { return process_current_pid() == PID_SHELL; }

static const command_t *lookup_command(const char *name) {
    const command_t *c = command_find(name);
    for (size_t i = 0; !c && i < sizeof(legacy_names) / sizeof(legacy_names[0]); i++)
        if (!strcmp(name, legacy_names[i].alias)) c = command_find(legacy_names[i].target);
    return c;
}

static void execute_line(const char *raw) {
    /* one command, a pipeline, or a list of them: see include/lineexec.h and include/pipeline.h */
    bool ran;
    int st = line_execute(raw, lookup_command, shell_owns_output, &ran);
    if (ran) { last_exit = st; sv_set_status(st); }
}

static int execute_for_script(const char *line) { execute_line(line); sv_set_status(last_exit); return last_exit; }


/* ---------- login ---------- */
static bool logged_in(void) { return sys_getuid() >= 0; }

/* PS1 (set with the 'prompt' command): \u = user, \w = working directory, \$ = '#' for root else '>'. */
static void print_custom_prompt(const char *ps1, const cred_info_t *ci) {
    for (size_t i = 0; ps1[i]; i++) {
        if (ps1[i] != '\\' || !ps1[i + 1]) { screen_putc(ps1[i]); continue; }
        char k = ps1[++i];
        if (k == 'u') screen_puts(ci->name);
        else if (k == 'w') { char cwd[FS_PATH_MAX]; screen_puts(sys_getcwd(cwd, sizeof cwd) >= 0 ? cwd : "?"); }
        else if (k == '$') screen_putc(ci->euid == (int)UID_ROOT ? '#' : '>');
        else { screen_putc('\\'); screen_putc(k); }
    }
}

static void print_prompt(void) {
    cred_info_t ci;
    const char *ps1 = var_get("PS1");
    if (ps1 && sys_getcred(&ci) == 0 && ci.depth > 0) { print_custom_prompt(ps1, &ci); return; }
    if (sys_getcred(&ci) == 0 && ci.depth > 0) {
        char cwd[FS_PATH_MAX], shown[48]; user_info_t ui;
        const char *home = sys_user_lookup(ci.name, -1, &ui) == 0 ? ui.home : NULL;
        if (sys_getcwd(cwd, sizeof cwd) < 0) { cwd[0] = '?'; cwd[1] = '\0'; }
        prompt_path(cwd, home, shown, sizeof shown);                 /* "~", "~/docs", ".../a/b" */
        screen_set_color(ci.euid == (int)UID_ROOT ? VGA_LIGHT_RED : VGA_LIGHT_GREEN, VGA_BLACK);
        screen_puts(ci.name);
        screen_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        screen_puts("@MiniOS:");
        screen_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        screen_puts(shown);
        screen_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        screen_puts("> ");
    } else screen_puts("nobody" PROMPT_SUFFIX);
}

/* The login prompt owns the keyboard directly: it must work before any command can run. */
static void read_login_line(char *out, size_t cap, bool echo) {
    size_t n = 0;
    for (;;) {
        int k = keyboard_getkey();
        /* the boot messages are the first thing to look back at: scrolling works at the login prompt too */
        if (k == KEY_PGUP) { screen_scrollback(20); continue; }
        if (k == KEY_PGDN) { screen_scrollback(-20); continue; }
        if (k == KEY_SCROLLUP) { screen_scrollback(3); continue; }
        if (k == KEY_SCROLLDN) { screen_scrollback(-3); continue; }
        if (screen_scrollback_active()) screen_scrollback_reset();
        if (k == '\n') break;
        if (k == '\b') { if (n) { n--; screen_backspace(); } }
        else if (k == KEY_CTRL_C) { while (n) { n--; screen_backspace(); } }
        else if (k >= ' ' && k < 127 && n + 1 < cap) { out[n++] = (char)k; screen_putc(echo ? (char)k : '*'); }
    }
    out[n] = '\0';
    screen_putc('\n');
}

static void login_screen(void) {
    char name[USER_NAME_MAX + 2], pw[USER_PASS_MAX + 4];
    for (;;) {
        screen_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        screen_puts("\nMiniOS login");
        screen_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        screen_puts("  (root, admin, student, guest; passwords: README.md)\n");
        screen_puts("login: ");    read_login_line(name, sizeof name, true);
        screen_puts("Password: "); read_login_line(pw, sizeof pw, false);
        int rc = sys_login(name, pw);
        for (size_t i = 0; i < sizeof pw; i++) ((volatile char *)pw)[i] = 0;
        if (rc == 0) break;
        screen_set_color(VGA_LIGHT_RED, VGA_BLACK);
        screen_puts(rc == SYS_ELOCKED ? "Account is locked. Ask an administrator to run 'unlockuser'.\n" : "Login incorrect.\n");
        screen_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        sys_sleep(1000);                                  /* slows down password guessing */
    }
    user_info_t u;
    if (sys_user_lookup(name, 0, &u) == 0) {
        sys_chdir(u.home);                                /* start in the home directory when the permissions allow it */
        screen_puts("\nWelcome, "); screen_puts(u.name); screen_puts(".\n");
    }
    int fd = sys_open("/etc/motd", SYS_O_READ);
    if (fd >= 0) { char text[96]; int n = sys_read(fd, text, sizeof text - 1); sys_close(fd); if (n > 0) { text[n] = '\0'; screen_puts(text); } }
    screen_putc('\n');
}

/* ---------- line editor ----------
 * The text and the cursor live in an edit_t (lib/lineedit.c). The screen only shows it: the row/column where the input starts is
 * remembered, and the line is redrawn from there after every change that is not simply "typed a character at the end". */
static edit_t ed;
static int in_row, in_col;                 /* screen position of the first character of the input */
static uint32_t in_scrolls;                /* screen_scroll_count() when in_row was taken: scrolling moves the input up */
static bool in_block;                      /* inside an unfinished if/for/while: the prompt is "> " */

static void input_start(void) {            /* call right after a prompt has been printed */
    int r, c; screen_state(&r, &c, NULL);
    in_row = r; in_col = c; in_scrolls = screen_scroll_count();
}
static void sync_start(void) {
    uint32_t now = screen_scroll_count();
    in_row -= (int)(now - in_scrolls); in_scrolls = now;
    if (in_row < 0) in_row = 0;
}
static void place_cursor(size_t pos) {
    int lin = in_col + (int)pos;
    screen_set_cursor(in_row + lin / 80, lin % 80);
}
static void redraw(size_t old_len) {       /* print the whole line again, blank out what a shorter line left behind, put the cursor back */
    sync_start();
    screen_set_cursor(in_row, in_col);
    for (size_t i = 0; i < ed.len; i++) screen_putc(ed.buf[i]);
    for (size_t i = ed.len; i < old_len; i++) screen_putc(' ');
    sync_start();
    place_cursor(ed.pos);
}
static void prompt_again(void) {           /* after the screen was cleared or a list was shown */
    if (in_block) screen_puts("> "); else print_prompt();
    input_start();
    for (size_t i = 0; i < ed.len; i++) screen_putc(ed.buf[i]);
    sync_start();
    place_cursor(ed.pos);
}

/* Loops poll this once per turn: Ctrl+C stops a runaway "while". Other keys typed meanwhile are discarded. */
static bool ctrl_c_pressed(void) {
    bool hit = false;
    while (keyboard_has_key()) if (keyboard_getkey() == KEY_CTRL_C) hit = true;
    return hit;
}

/* Tab: complete the word before the cursor (command, $variable or file name). */
static void do_complete(void) {
    static comp_result_t r;                /* about 3 KB: kept off the stack */
    comp_kind_t kind = complete_word(ed.buf, ed.pos, complete_env_system(), &r);
    if (kind == COMP_NONE) return;
    if (kind == COMP_UNIQUE || kind == COMP_PARTIAL) {
        char text[COMP_TEXT_MAX + 2]; size_t n = 0;
        for (; r.text[n]; n++) text[n] = r.text[n];
        if (kind == COMP_UNIQUE && r.append_space && (ed.pos >= ed.len || ed.buf[ed.pos] != ' ')) text[n++] = ' ';
        text[n] = '\0';
        size_t old = ed.len;
        if (edit_replace(&ed, r.word_start, ed.pos, text)) redraw(old);
        return;
    }
    /* several names and nothing more to add: show them, then the prompt and the line again */
    place_cursor(ed.len);
    screen_putc('\n');
    size_t shown = r.ncand, width = 0;
    for (size_t i = 0; i < shown; i++) { size_t l = 0; while (r.cand[i][l]) l++; if (r.cand_dir[i]) l++; if (l > width) width = l; }
    width += 2; if (width > 78) width = 78;
    size_t cols = 80 / width; if (cols == 0) cols = 1;
    for (size_t i = 0; i < shown; i++) {
        size_t l = 0; while (r.cand[i][l]) l++;
        if (r.cand_dir[i]) screen_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        screen_puts(r.cand[i]);
        if (r.cand_dir[i]) { screen_putc('/'); l++; screen_set_color(VGA_LIGHT_GREY, VGA_BLACK); }
        if ((i + 1) % cols == 0 || i + 1 == shown) screen_putc('\n');
        else for (size_t k = l; k < width; k++) screen_putc(' ');
    }
    if (r.total > r.ncand) { screen_put_dec((uint32_t)(r.total - r.ncand)); screen_puts(" more not shown\n"); }
    prompt_again();
}

#define SCROLL_STEP 20

void shell_run(void) {
    char block[512];                 /* an if/for/while being typed over several lines, joined with " ; " */
    size_t blen = 0;
    int nav = -1;                    /* history position while browsing; -1 = new line */

    history_init();
    shellenv_set_executor(execute_for_script);       /* lets 'source' and 'demo' run command lines through the real shell */
    line_set_interrupt_check(ctrl_c_pressed);
    login_screen();
    print_prompt();
    edit_init(&ed); input_start();

    for (;;) {
        int k = keyboard_getkey();

        /* PageUp / PageDown look through the lines that scrolled off the screen; any other key returns to the prompt first */
        if (k == KEY_PGUP) { screen_scrollback(SCROLL_STEP); continue; }
        if (k == KEY_PGDN) { screen_scrollback(-SCROLL_STEP); continue; }
        if (k == KEY_SCROLLUP) { screen_scrollback(3); continue; }           /* Ctrl+Up / Ctrl+Down: three lines at a time */
        if (k == KEY_SCROLLDN) { screen_scrollback(-3); continue; }
        if (screen_scrollback_active()) screen_scrollback_reset();

        if (k == '\n') {
            place_cursor(ed.len);                     /* the cursor may be in the middle of the line */
            screen_putc('\n');
            /* an unfinished if/for/while: keep reading lines ("> " prompt) until its fi/done, then run it as one statement */
            char stmt[512];
            size_t sl = 0, ll = ed.len;
            if (blen && ll == 0) { screen_puts("> "); input_start(); nav = -1; continue; }
            if (blen + ll + 4 >= sizeof stmt) { screen_puts("Error: the block is too long (limit 500 characters); it was discarded.\n"); blen = 0; in_block = false; edit_init(&ed); nav = -1; print_prompt(); input_start(); continue; }
            if (blen) { for (size_t i = 0; i < blen; i++) stmt[sl++] = block[i]; stmt[sl++] = ' '; stmt[sl++] = ';'; stmt[sl++] = ' '; }
            for (size_t i = 0; i < ll; i++) stmt[sl++] = ed.buf[i];
            stmt[sl] = '\0';
            if (command_list_incomplete(stmt)) {
                for (size_t i = 0; i <= sl; i++) block[i] = stmt[i];
                blen = sl; in_block = true; edit_init(&ed); nav = -1;
                screen_puts("> "); input_start();
                continue;
            }
            if (blen == 0) history_add(ed.buf);
            else if (sl < SHELL_LINE_MAX) history_add(stmt);
            blen = 0; in_block = false;
            execute_line(stmt);
            sv_set_status(last_exit);
            edit_init(&ed); nav = -1;
            if (!logged_in()) login_screen();       /* 'logout' ends the session: back to the login prompt */
            print_prompt();
            input_start();
        } else if (k == KEY_CTRL_C) {
            place_cursor(ed.len);
            screen_puts("^C\n"); edit_init(&ed); nav = -1; blen = 0; in_block = false; print_prompt(); input_start();
        } else if (k == KEY_CTRL_L) {
            screen_clear(); prompt_again();
        } else if (k == '\b') {
            bool at_end = ed.pos == ed.len; size_t old = ed.len;
            if (edit_backspace(&ed)) { if (at_end) screen_backspace(); else redraw(old); }
        } else if (k == KEY_DELETE) {
            size_t old = ed.len; if (edit_delete(&ed)) redraw(old);
        } else if (k == KEY_LEFT)  { if (edit_left(&ed))  place_cursor(ed.pos); }
        else if (k == KEY_RIGHT)   { if (edit_right(&ed)) place_cursor(ed.pos); }
        else if (k == KEY_HOME || k == 1) { if (edit_home(&ed)) place_cursor(ed.pos); }        /* Ctrl+A */
        else if (k == KEY_END  || k == 5) { if (edit_end(&ed))  place_cursor(ed.pos); }        /* Ctrl+E */
        else if (k == 11) { size_t old = ed.len; if (edit_kill_to_end(&ed)) redraw(old); }      /* Ctrl+K */
        else if (k == 21) { size_t old = ed.len; if (edit_kill_line(&ed)) redraw(old); }        /* Ctrl+U */
        else if (k == 23) { size_t old = ed.len; if (edit_kill_word(&ed)) redraw(old); }        /* Ctrl+W */
        else if (k == '\t') { do_complete(); }
        else if (k == KEY_UP) {
            int cnt = history_count();
            if (cnt == 0) continue;
            if (nav < 0) nav = cnt - 1; else if (nav > 0) nav--;
            size_t old = ed.len; edit_set(&ed, history_get(nav)); redraw(old);
        } else if (k == KEY_DOWN) {
            if (nav < 0) continue;
            size_t old = ed.len;
            if (nav < history_count() - 1) { nav++; edit_set(&ed, history_get(nav)); }
            else { nav = -1; edit_set(&ed, ""); }
            redraw(old);
        } else if (k >= ' ' && k < 127) {
            bool at_end = ed.pos == ed.len; size_t old = ed.len;
            if (edit_insert(&ed, (char)k)) { if (at_end) screen_putc((char)k); else redraw(old); }
        }
    }
}
