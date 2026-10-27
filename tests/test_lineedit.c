/* Unit tests for lib/lineedit.c, lib/scrollback.c and lib/complete.c (pure logic behind the Phase 16 console UI). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/lineedit.h"
#include "include/keyboard.h"
#include "include/scrollback.h"
#include "include/complete.h"

static int run, failed;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; printf("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static bool is(const edit_t *e, const char *text, size_t pos) { return !strcmp(e->buf, text) && e->len == strlen(text) && e->pos == pos; }
static void type(edit_t *e, const char *s) { while (*s) edit_insert(e, *s++); }

static void test_edit(void) {
    puts("edit: insert, cursor, delete");
    edit_t e; edit_init(&e);
    CHECK(is(&e, "", 0) && !edit_backspace(&e) && !edit_delete(&e) && !edit_left(&e) && !edit_right(&e) && !edit_home(&e) && !edit_end(&e));
    type(&e, "hello"); CHECK(is(&e, "hello", 5));
    CHECK(edit_left(&e) && edit_left(&e) && is(&e, "hello", 3));
    CHECK(edit_insert(&e, 'X') && is(&e, "helXlo", 4));                       /* typed in the middle */
    CHECK(edit_backspace(&e) && is(&e, "hello", 3));
    CHECK(edit_delete(&e) && is(&e, "helo", 3) && edit_delete(&e) && is(&e, "hel", 3) && !edit_delete(&e));
    CHECK(edit_home(&e) && is(&e, "hel", 0) && !edit_home(&e) && !edit_backspace(&e) && is(&e, "hel", 0));
    CHECK(edit_insert(&e, '>') && is(&e, ">hel", 1));
    CHECK(edit_end(&e) && is(&e, ">hel", 4) && !edit_end(&e) && !edit_right(&e));
    CHECK(edit_right(&e) == false && edit_left(&e) && edit_right(&e) && is(&e, ">hel", 4));
    puts("edit: only printable characters, and the line has a limit");
    edit_init(&e);
    CHECK(!edit_insert(&e, '\n') && !edit_insert(&e, '\t') && !edit_insert(&e, 0) && !edit_insert(&e, 127) && !edit_insert(&e, (char)200) && is(&e, "", 0));
    for (int i = 0; i < EDIT_MAX - 1; i++) CHECK(edit_insert(&e, 'a'));
    CHECK(e.len == EDIT_MAX - 1 && !edit_insert(&e, 'b') && strlen(e.buf) == EDIT_MAX - 1);
    edit_home(&e); CHECK(!edit_insert(&e, 'b') && e.buf[0] == 'a');             /* full is full, wherever the cursor is */
    CHECK(edit_backspace(&e) == false && edit_delete(&e) && e.len == EDIT_MAX - 2 && edit_insert(&e, 'Z') && e.buf[0] == 'Z');
    puts("edit: kill commands");
    edit_init(&e); type(&e, "one two three"); edit_left(&e); edit_left(&e);
    CHECK(edit_kill_to_end(&e) && is(&e, "one two thr", 11) && !edit_kill_to_end(&e));
    edit_home(&e); edit_right(&e); edit_right(&e); CHECK(edit_kill_to_end(&e) && is(&e, "on", 2));
    edit_init(&e); type(&e, "one two three");
    CHECK(edit_kill_word(&e) && is(&e, "one two ", 8) && edit_kill_word(&e) && is(&e, "one ", 4) && edit_kill_word(&e) && is(&e, "", 0) && !edit_kill_word(&e));
    edit_init(&e); type(&e, "abc def"); edit_left(&e); edit_left(&e);                 /* cursor between 'd' and 'e'... on "de|f" */
    CHECK(edit_kill_word(&e) && is(&e, "abc ef", 4));                         /* only the part of the word BEFORE the cursor goes */
    edit_init(&e); type(&e, "   x"); edit_home(&e); CHECK(!edit_kill_word(&e));
    edit_init(&e); type(&e, "a  "); CHECK(edit_kill_word(&e) && is(&e, "", 0));            /* trailing spaces go with the word */
    edit_init(&e); type(&e, "abc"); edit_left(&e); CHECK(edit_kill_line(&e) && is(&e, "", 0) && !edit_kill_line(&e));
    puts("edit: set (history) and replace (completion)");
    edit_init(&e); type(&e, "old text");
    CHECK(edit_set(&e, "newer") && is(&e, "newer", 5) && edit_set(&e, "") && is(&e, "", 0));
    char longt[300]; memset(longt, 'q', 299); longt[299] = 0; edit_set(&e, longt); CHECK(e.len == EDIT_MAX - 1 && e.pos == e.len && strlen(e.buf) == EDIT_MAX - 1);
    edit_init(&e); type(&e, "ls /et");
    CHECK(edit_replace(&e, 3, 6, "/etc/") && is(&e, "ls /etc/", 8));
    edit_init(&e); type(&e, "ec foo"); edit_home(&e); edit_right(&e); edit_right(&e);                      /* cursor after "ec" */
    CHECK(edit_replace(&e, 0, 2, "echo ") && is(&e, "echo  foo", 5));                                    /* the tail stays */
    edit_init(&e); type(&e, "cat longfilename x"); CHECK(edit_replace(&e, 4, 16, "l") && is(&e, "cat l x", 5));        /* shrinking */
    edit_init(&e); type(&e, "abc"); CHECK(edit_replace(&e, 1, 1, "XY") && is(&e, "aXYbc", 3) && edit_replace(&e, 0, 5, "") && is(&e, "", 0));
    edit_init(&e); type(&e, "abc"); CHECK(!edit_replace(&e, 2, 1, "x") && !edit_replace(&e, 0, 9, "x") && is(&e, "abc", 3));
    edit_init(&e); edit_set(&e, longt); e.buf[EDIT_MAX - 1] = 0; edit_set(&e, "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456");
    CHECK(!edit_replace(&e, 0, 1, "too long to fit") && e.len == EDIT_MAX - 1);
    CHECK(!edit_replace(&e, 0, 0, "xx") && e.len == EDIT_MAX - 1);
    CHECK(edit_replace(&e, 0, 4, "xy") && e.len == EDIT_MAX - 3 && e.buf[0] == 'x');
    for (int i = 0; i < 200; i++) { edit_init(&e); type(&e, "echo hello"); edit_replace(&e, 5, 10, "world"); if (strcmp(e.buf, "echo world")) { CHECK(false); break; } }
    puts("edit: a long random session never breaks the invariants");
    srand(7); edit_init(&e);
    for (int i = 0; i < 20000; i++) {
        switch (rand() % 9) {
            case 0: case 1: case 2: edit_insert(&e, (char)('a' + rand() % 26)); break;
            case 3: edit_backspace(&e); break; case 4: edit_delete(&e); break;
            case 5: edit_left(&e); break; case 6: edit_right(&e); break;
            case 7: if (rand() % 20 == 0) edit_kill_word(&e); else edit_home(&e); break;
            default: if (rand() % 30 == 0) edit_kill_to_end(&e); else edit_end(&e);
        }
        if (e.len >= EDIT_MAX || e.pos > e.len || e.buf[e.len] != '\0' || strlen(e.buf) != e.len) { CHECK(false); break; }
    }
    CHECK(e.pos <= e.len && strlen(e.buf) == e.len);
}

static void test_key_names(void) {
    puts("key_name (what kbdinfo shows)");
    char o[32];
    CHECK(key_name(KEY_PGUP, o, sizeof o) == 6 && !strcmp(o, "PageUp") && key_name(KEY_PGDN, o, sizeof o) && !strcmp(o, "PageDown"));
    key_name(KEY_UP, o, sizeof o); CHECK(!strcmp(o, "Up")); key_name(KEY_DOWN, o, sizeof o); CHECK(!strcmp(o, "Down"));
    key_name(KEY_LEFT, o, sizeof o); CHECK(!strcmp(o, "Left")); key_name(KEY_RIGHT, o, sizeof o); CHECK(!strcmp(o, "Right"));
    key_name(KEY_DELETE, o, sizeof o); CHECK(!strcmp(o, "Delete")); key_name(KEY_HOME, o, sizeof o); CHECK(!strcmp(o, "Home")); key_name(KEY_END, o, sizeof o); CHECK(!strcmp(o, "End"));
    key_name(KEY_SCROLLUP, o, sizeof o); CHECK(strstr(o, "Ctrl+Up")); key_name(KEY_SCROLLDN, o, sizeof o); CHECK(strstr(o, "Ctrl+Down"));
    key_name('\n', o, sizeof o); CHECK(!strcmp(o, "Enter")); key_name('\b', o, sizeof o); CHECK(!strcmp(o, "Backspace")); key_name('\t', o, sizeof o); CHECK(!strcmp(o, "Tab"));
    key_name(27, o, sizeof o); CHECK(!strcmp(o, "Esc")); key_name(3, o, sizeof o); CHECK(!strcmp(o, "Ctrl+C")); key_name(12, o, sizeof o); CHECK(!strcmp(o, "Ctrl+L")); key_name(1, o, sizeof o); CHECK(!strcmp(o, "Ctrl+A"));
    key_name('a', o, sizeof o); CHECK(!strcmp(o, "a")); key_name('Z', o, sizeof o); CHECK(!strcmp(o, "Z")); key_name(' ', o, sizeof o); CHECK(!strcmp(o, "Space"));
    key_name(0, o, sizeof o); CHECK(!strcmp(o, "?")); key_name(0x500, o, sizeof o); CHECK(!strcmp(o, "?")); key_name(-1, o, sizeof o); CHECK(!strcmp(o, "?")); key_name(127, o, sizeof o); CHECK(!strcmp(o, "?"));
    char tiny[4]; CHECK(key_name(KEY_PGUP, tiny, sizeof tiny) == 3 && !strcmp(tiny, "Pag") && key_name(KEY_PGUP, tiny, 0) == 0);
}

static void test_prompt_path(void) {
    puts("prompt_path");
    char o[64];
    CHECK(prompt_path("/", "/home/admin", o, sizeof o) == 1 && !strcmp(o, "/"));
    prompt_path("/home/admin", "/home/admin", o, sizeof o); CHECK(!strcmp(o, "~"));
    prompt_path("/home/admin/docs", "/home/admin", o, sizeof o); CHECK(!strcmp(o, "~/docs"));
    prompt_path("/home/admin/a/b", "/home/admin", o, sizeof o); CHECK(!strcmp(o, "~/a/b"));
    prompt_path("/home/administrator", "/home/admin", o, sizeof o); CHECK(!strcmp(o, "/home/administrator"));       /* not inside home: only a name prefix */
    prompt_path("/home/admin2/x", "/home/admin", o, sizeof o); CHECK(!strcmp(o, "/home/admin2/x"));
    prompt_path("/tmp", "/home/admin", o, sizeof o); CHECK(!strcmp(o, "/tmp"));
    prompt_path("/home", "/home/admin", o, sizeof o); CHECK(!strcmp(o, "/home"));
    prompt_path("/tmp", NULL, o, sizeof o); CHECK(!strcmp(o, "/tmp"));
    prompt_path("/tmp", "", o, sizeof o); CHECK(!strcmp(o, "/tmp"));
    prompt_path("/anything", "/", o, sizeof o); CHECK(!strcmp(o, "/anything"));                                    /* a home of "/" is not abbreviated */
    prompt_path("/root", "/root", o, sizeof o); CHECK(!strcmp(o, "~"));
    prompt_path(NULL, "/home/a", o, sizeof o); CHECK(!strcmp(o, "?")); prompt_path("", "/home/a", o, sizeof o); CHECK(!strcmp(o, "?"));
    prompt_path("/usr/local/share/doc/package/files", "/home/admin", o, sizeof o); CHECK(!strcmp(o, ".../package/files"));
    prompt_path("/a/b/c", "/home/admin", o, sizeof o); CHECK(!strcmp(o, "/a/b/c"));
    prompt_path("/home/admin/very/long/directory/structure/here", "/home/admin", o, sizeof o); CHECK(!strcmp(o, ".../structure/here"));
    prompt_path("/abcdefghijklmnopqrstuvwxyz0123", "/home/admin", o, sizeof o); CHECK(!strcmp(o, "/abcdefghijklmnopqrstuvwxyz0123"));            /* long but no two slashes: kept */
    char tiny[6]; size_t n = prompt_path("/home/admin/docs", "/home/admin", tiny, sizeof tiny); CHECK(n < 6 && strlen(tiny) < 6 && !strncmp(tiny, "~/doc", 5));
    CHECK(prompt_path("/x", "/home/a", o, 0) == 0);
}

static uint16_t rowof(char c) { return (uint16_t)(0x0700 | (uint8_t)c); }
static void fillrow(uint16_t *row, char c) { for (int i = 0; i < SB_COLS; i++) row[i] = rowof(c); }

static void test_scrollback(void) {
    puts("scrollback ring");
    static sb_t s; uint16_t row[SB_COLS]; sb_init(&s);
    CHECK(sb_count(&s) == 0 && sb_get(&s, 0) == NULL && sb_clamp(&s, 5) == 0 && sb_clamp(&s, -3) == 0);
    fillrow(row, 'a'); sb_push(&s, row); fillrow(row, 'b'); sb_push(&s, row); fillrow(row, 'c'); sb_push(&s, row);
    CHECK(sb_count(&s) == 3 && (sb_get(&s, 0)[0] & 0xFF) == 'a' && (sb_get(&s, 2)[79] & 0xFF) == 'c' && sb_get(&s, 3) == NULL && sb_get(&s, -1) == NULL);
    CHECK(sb_clamp(&s, 2) == 2 && sb_clamp(&s, 3) == 3 && sb_clamp(&s, 4) == 3 && sb_clamp(&s, -1) == 0);
    puts("a full ring drops the oldest line");
    for (int i = 0; i < 300; i++) { fillrow(row, (char)('A' + i % 26)); row[0] = (uint16_t)(0x0700 | (uint8_t)('0' + 0)); row[1] = (uint16_t)i; sb_push(&s, row); }
    CHECK(sb_count(&s) == SB_LINES && sb_get(&s, 0)[1] == 100 && sb_get(&s, SB_LINES - 1)[1] == 299 && sb_get(&s, SB_LINES) == NULL);
    bool ordered = true; for (int i = 1; i < SB_LINES; i++) if (sb_get(&s, i)[1] != sb_get(&s, i - 1)[1] + 1) ordered = false; CHECK(ordered);

    puts("which rows to draw while looking back");
    sb_init(&s);
    for (int i = 0; i < 10; i++) { fillrow(row, (char)('0' + i)); sb_push(&s, row); }              /* history: 0..9 */
    static uint16_t live[25 * SB_COLS]; for (int r = 0; r < 25; r++) fillrow(live + r * SB_COLS, (char)('a' + r));   /* live screen: a..y */
    CHECK((sb_view_row(&s, live, 25, 0, 0)[0] & 0xFF) == 'a' && (sb_view_row(&s, live, 25, 0, 24)[0] & 0xFF) == 'y');                 /* off 0 = the live screen */
    CHECK((sb_view_row(&s, live, 25, 1, 0)[0] & 0xFF) == '9' && (sb_view_row(&s, live, 25, 1, 1)[0] & 0xFF) == 'a' && (sb_view_row(&s, live, 25, 1, 24)[0] & 0xFF) == 'x');
    CHECK((sb_view_row(&s, live, 25, 10, 0)[0] & 0xFF) == '0' && (sb_view_row(&s, live, 25, 10, 9)[0] & 0xFF) == '9' && (sb_view_row(&s, live, 25, 10, 10)[0] & 0xFF) == 'a');
    CHECK((sb_view_row(&s, live, 25, 99, 0)[0] & 0xFF) == '0');                                      /* an offset past the history is clamped */
    CHECK((sb_view_row(&s, live, 25, -5, 0)[0] & 0xFF) == 'a');
    for (int off = 0; off <= 10; off++) for (int r = 0; r < 25; r++) if (!sb_view_row(&s, live, 25, off, r)) CHECK(false);

    puts("clearing the screen keeps its text");
    sb_init(&s);
    for (int r = 0; r < 25; r++) { fillrow(live + r * SB_COLS, ' '); }
    fillrow(live, 'X'); fillrow(live + 3 * SB_COLS, 'Y');
    sb_push_screen(&s, live, 25);
    CHECK(sb_count(&s) == 4 && (sb_get(&s, 0)[0] & 0xFF) == 'X' && (sb_get(&s, 3)[0] & 0xFF) == 'Y');   /* rows 0..3: up to the last row with text */
    sb_init(&s);
    for (int r = 0; r < 25; r++) { fillrow(live + r * SB_COLS, ' '); }
    sb_push_screen(&s, live, 25); CHECK(sb_count(&s) == 0);                                              /* an empty screen leaves nothing behind */
    for (int r = 0; r < 25; r++) { for (int c = 0; c < SB_COLS; c++) live[r * SB_COLS + c] = 0; }
    sb_push_screen(&s, live, 25); CHECK(sb_count(&s) == 0);
    for (int r = 0; r < 25; r++) { fillrow(live + r * SB_COLS, 'z'); }
    sb_push_screen(&s, live, 25); CHECK(sb_count(&s) == 25);
}

/* ---------- completion with fake lists ---------- */
static const char *const cmds[] = { "echo", "ecasound", "ls", "cat", "clear", "calc", "cal", "sudo", "sort", "sleep", "format" };
static const char *const vars[] = { "HOME", "HOSTNAME", "PATH", "PS1", "USER" };
static void emit_cmds(comp_emit_fn emit, void *ctx) { for (size_t i = 0; i < sizeof cmds / sizeof *cmds; i++) emit(cmds[i], false, ctx); }
static void emit_vars(comp_emit_fn emit, void *ctx) { for (size_t i = 0; i < sizeof vars / sizeof *vars; i++) emit(vars[i], false, ctx); }
static void emit_entries(const char *dir, comp_emit_fn emit, void *ctx) {
    if (!strcmp(dir, "")) { emit("docs", true, ctx); emit("notes.txt", false, ctx); emit("notes2.txt", false, ctx); emit(".hidden", false, ctx); emit("my file", false, ctx); emit("a;b", false, ctx); emit("src", true, ctx); }
    else if (!strcmp(dir, "/etc/")) { emit("passwd", false, ctx); emit("motd", false, ctx); emit("hostname", false, ctx); }
    else if (!strcmp(dir, "docs/")) { emit("readme.md", false, ctx); emit("report.md", false, ctx); emit("sub", true, ctx); }
    else if (!strcmp(dir, "/")) { emit("etc", true, ctx); emit("tmp", true, ctx); emit("home", true, ctx); }
}
static void big_entries(const char *dir, comp_emit_fn emit, void *ctx) { (void)dir; for (int i = 0; i < 100; i++) { char n[12]; snprintf(n, sizeof n, "f%03d", i); emit(n, false, ctx); } }
static comp_env_t env = { emit_cmds, emit_vars, emit_entries };
static comp_result_t R;
static comp_kind_t comp(const char *line) { return complete_word(line, strlen(line), &env, &R); }

static void test_complete(void) {
    puts("complete: commands");
    CHECK(comp("ech") == COMP_UNIQUE && !strcmp(R.text, "echo") && R.append_space && R.word_start == 0);
    CHECK(comp("l") == COMP_UNIQUE && !strcmp(R.text, "ls"));
    CHECK(comp("ca") == COMP_LIST && R.ncand == 3 && !strcmp(R.cand[0], "cal") && !strcmp(R.cand[1], "calc") && !strcmp(R.cand[2], "cat"));     /* nothing more to add: a list */
    CHECK(comp("cal") == COMP_LIST && R.ncand == 2);                                                            /* "cal" is a command AND the start of "calc" */
    CHECK(comp("cl") == COMP_UNIQUE && !strcmp(R.text, "clear"));
    CHECK(comp("c") == COMP_LIST && R.ncand == 5 && !strcmp(R.cand[0], "cal") && !strcmp(R.cand[1], "calc") && !strcmp(R.cand[2], "cat") && !strcmp(R.cand[3], "clear") && !strcmp(R.cand[4], "continue"));   /* sorted, keywords included */
    CHECK(comp("ec") == COMP_LIST && R.ncand == 2 && !strcmp(R.cand[0], "ecasound") && !strcmp(R.cand[1], "echo"));
    CHECK(comp("eca") == COMP_UNIQUE && !strcmp(R.text, "ecasound"));
    CHECK(comp("s") == COMP_LIST && R.ncand == 3 && !strcmp(R.cand[0], "sleep") && !strcmp(R.cand[1], "sort") && !strcmp(R.cand[2], "sudo"));
    CHECK(comp("so") == COMP_UNIQUE && !strcmp(R.text, "sort"));
    CHECK(comp("zzz") == COMP_NONE && R.ncand == 0);
    CHECK(comp("") == COMP_LIST && R.total >= 11);                                                  /* empty word: everything, capped */
    CHECK(R.ncand == COMP_MAX || R.ncand == R.total);
    puts("complete: keywords and command positions");
    CHECK(comp("wh") == COMP_UNIQUE && !strcmp(R.text, "while") && R.append_space);
    CHECK(comp("fo") == COMP_PARTIAL && !strcmp(R.text, "for"));                                              /* "for" is also the start of "format": complete as far as they agree */
    CHECK(comp("for") == COMP_LIST && R.ncand == 2 && !strcmp(R.cand[0], "for") && !strcmp(R.cand[1], "format"));
    CHECK(comp("echo hi ; ls") == COMP_UNIQUE && !strcmp(R.text, "ls") && R.word_start == 10);
    CHECK(comp("echo hi && so") == COMP_UNIQUE && !strcmp(R.text, "sort"));
    CHECK(comp("echo hi || ca") == COMP_LIST && R.ncand == 3 && R.word_start == 11);
    CHECK(comp("cat f | so") == COMP_UNIQUE && !strcmp(R.text, "sort"));
    CHECK(comp("if tr") == COMP_NONE);
    CHECK(comp("if so") == COMP_UNIQUE && !strcmp(R.text, "sort") && comp("if true ; then ec") != COMP_NONE);
    CHECK(comp("sudo so") == COMP_UNIQUE && !strcmp(R.text, "sort"));
    CHECK(comp("for x in a ; do ec") != COMP_NONE && comp("while ca") != COMP_NONE);
    CHECK(comp("echo ec") == COMP_NONE);                                                           /* after echo it is a file name, not a command */
    puts("complete: file names");
    CHECK(comp("ls do") == COMP_UNIQUE && !strcmp(R.text, "docs/") && !R.append_space && R.word_start == 3);        /* a directory gets a slash and no space */
    CHECK(comp("ls no") == COMP_PARTIAL && !strcmp(R.text, "notes"));                                             /* notes.txt, notes2.txt share "notes" */
    CHECK(comp("ls notes.") == COMP_UNIQUE && !strcmp(R.text, "notes.txt") && R.append_space);
    CHECK(comp("ls notes") == COMP_LIST && R.ncand == 2 && !strcmp(R.cand[0], "notes.txt") && !strcmp(R.cand[1], "notes2.txt"));
    CHECK(comp("ls /etc/pa") == COMP_UNIQUE && !strcmp(R.text, "/etc/passwd") && R.word_start == 3);
    CHECK(comp("ls /et") == COMP_UNIQUE && !strcmp(R.text, "/etc/"));
    CHECK(comp("ls /e") == COMP_UNIQUE && !strcmp(R.text, "/etc/") && !R.append_space);
    CHECK(comp("ls /etc/") == COMP_LIST && R.ncand == 3 && !strcmp(R.cand[0], "hostname") && !strcmp(R.cand[1], "motd") && !strcmp(R.cand[2], "passwd"));
    CHECK(comp("ls /etc/h") == COMP_UNIQUE && !strcmp(R.text, "/etc/hostname"));
    CHECK(comp("cat docs/re") == COMP_LIST && R.ncand == 2);
    CHECK(comp("cat docs/rea") == COMP_UNIQUE && !strcmp(R.text, "docs/readme.md"));
    CHECK(comp("cat docs/r") == COMP_PARTIAL && !strcmp(R.text, "docs/re"));
    CHECK(comp("cat docs/re") == COMP_LIST && !strcmp(R.cand[0], "readme.md") && !strcmp(R.cand[1], "report.md"));
    CHECK(comp("cat docs/s") == COMP_UNIQUE && !strcmp(R.text, "docs/sub/"));
    CHECK(comp("cd /") == COMP_LIST && R.ncand == 3);
    CHECK(comp("cat nothing") == COMP_NONE && comp("cat docs/zzz") == COMP_NONE && comp("cat /nodir/x") == COMP_NONE);
    puts("complete: hidden and unsafe names are not offered");
    CHECK(comp("ls .h") == COMP_UNIQUE && !strcmp(R.text, ".hidden"));
    CHECK(comp("ls .") == COMP_UNIQUE && !strcmp(R.text, ".hidden"));
    comp("ls "); bool leaked = false; for (size_t i = 0; i < R.ncand; i++) if (R.cand[i][0] == '.' || strchr(R.cand[i], ' ') || strchr(R.cand[i], ';')) leaked = true; CHECK(!leaked && R.ncand == 4);
    CHECK(comp("ls my") == COMP_NONE && comp("ls a") == COMP_NONE);
    CHECK(comp_name_safe("file.txt") && comp_name_safe("a-b_c") && !comp_name_safe("a b") && !comp_name_safe("a;b") && !comp_name_safe("a|b") && !comp_name_safe("a\"b") && !comp_name_safe("a'b") && !comp_name_safe("$x") && !comp_name_safe("a>b") && !comp_name_safe("a&b") && !comp_name_safe("a*") && !comp_name_safe("") && !comp_name_safe(NULL) && !comp_name_safe("tab\there") && !comp_name_safe("a\\b"));
    puts("complete: after > and <");
    CHECK(comp("echo x > no") == COMP_PARTIAL && !strcmp(R.text, "notes"));
    CHECK(comp("echo x >do") == COMP_UNIQUE && !strcmp(R.text, "docs/"));
    CHECK(comp("sort < no") == COMP_PARTIAL);
    CHECK(comp("echo x >> do") == COMP_UNIQUE && !strcmp(R.text, "docs/"));
    puts("complete: variables");
    CHECK(comp("echo $US") == COMP_UNIQUE && !strcmp(R.text, "$USER") && !R.append_space && R.word_start == 5);
    CHECK(comp("echo $H") == COMP_PARTIAL && !strcmp(R.text, "$HO"));
    CHECK(comp("echo $HO") == COMP_LIST && R.ncand == 2 && !strcmp(R.cand[0], "HOME") && !strcmp(R.cand[1], "HOSTNAME"));
    CHECK(comp("echo $HOM") == COMP_UNIQUE && !strcmp(R.text, "$HOME"));
    CHECK(comp("echo ${PA") == COMP_UNIQUE && !strcmp(R.text, "${PATH}"));
    CHECK(comp("echo $") == COMP_LIST && R.ncand == 5);
    CHECK(comp("echo $ZZ") == COMP_NONE);
    CHECK(comp("cat $HO") == COMP_LIST);
    puts("complete: the cursor can be in the middle, quotes are left alone");
    const char *line = "ls do ; echo hi"; CHECK(complete_word(line, 5, &env, &R) == COMP_UNIQUE && !strcmp(R.text, "docs/") && R.word_start == 3);
    CHECK(complete_word("ech x", 3, &env, &R) == COMP_UNIQUE && !strcmp(R.text, "echo") && R.word_start == 0);
    CHECK(comp("echo \"no") == COMP_NONE && comp("echo 'no") == COMP_NONE && comp("echo a\\no") == COMP_NONE);
    CHECK(complete_word("", 0, &env, &R) == COMP_LIST);
    CHECK(complete_word("ls ", 3, &env, &R) == COMP_LIST && R.word_start == 3);
    comp_env_t empty = { NULL, NULL, NULL };
    CHECK(complete_word("ec", 2, &empty, &R) == COMP_NONE && complete_word("$H", 2, &empty, &R) == COMP_NONE && complete_word("ls n", 4, &empty, &R) == COMP_NONE);
    puts("complete: a big directory is capped and counted");
    comp_env_t bigenv = { emit_cmds, emit_vars, big_entries };
    CHECK(complete_word("ls f", 4, &bigenv, &R) == COMP_PARTIAL && !strcmp(R.text, "f0"));
    CHECK(complete_word("ls f0", 5, &bigenv, &R) == COMP_LIST && R.ncand == COMP_MAX && R.total == 100);
    bool sorted = true; for (size_t i = 1; i < R.ncand; i++) if (strcmp(R.cand[i - 1], R.cand[i]) >= 0) sorted = false; CHECK(sorted);
    CHECK(complete_word("ls f099", 7, &bigenv, &R) == COMP_UNIQUE && !strcmp(R.text, "f099"));
    puts("complete: limits");
    char longw[300]; memset(longw, 'a', 299); longw[299] = 0; CHECK(comp(longw) == COMP_NONE);
    CHECK(complete_word("x", 0, &env, &R) != COMP_NONE);                                              /* an empty word at 0 lists commands */
}

int main(void) {
    test_edit(); test_key_names(); test_prompt_path(); test_scrollback(); test_complete();
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
