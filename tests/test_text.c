/* Unit tests for lib/glob.c, lib/textutil.c and kernel/device/capture.c (pure logic). */
#include <stdio.h>
#include <string.h>
#include "include/glob.h"
#include "include/textutil.h"
#include "include/capture.h"

static int run, failed;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; printf("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static void test_glob(void) {
    puts("glob");
    CHECK(glob_match("abc", "abc") && !glob_match("abc", "abd") && !glob_match("abc", "ab") && !glob_match("ab", "abc"));
    CHECK(glob_match("*", "") && glob_match("*", "anything") && glob_match("**", "x") && glob_match("***", ""));
    CHECK(glob_match("a*", "a") && glob_match("a*", "abc") && !glob_match("a*", "ba"));
    CHECK(glob_match("*c", "abc") && !glob_match("*c", "abcd") && glob_match("*.txt", "notes.txt") && !glob_match("*.txt", "notes.txt.bak"));
    CHECK(glob_match("a*c", "abc") && glob_match("a*c", "ac") && glob_match("a*c", "abcbc") && !glob_match("a*c", "abcb"));
    CHECK(glob_match("?", "x") && !glob_match("?", "") && !glob_match("?", "xy") && glob_match("h?me", "home") && !glob_match("h?me", "hme"));
    CHECK(glob_match("*a*b*c*", "xxaxxbxxcxx") && !glob_match("*a*b*c*", "xxcxxbxxaxx"));
    CHECK(glob_match("", "") && !glob_match("", "a") && !glob_match(NULL, "a") && !glob_match("a", NULL));
    CHECK(glob_match("*ab", "aab") && glob_match("*aab", "aaab") && !glob_match("*a?b", "ab"));      /* backtracking cases */
    CHECK(!glob_match("a", "A"));                                                                    /* case-sensitive */
    char longs[1000]; memset(longs, 'a', 999); longs[999] = 0;
    CHECK(glob_match("*a", longs) && glob_match("a*a*a*a*a*b", "aaaaaaaaaaaaaab") && !glob_match("a*a*a*a*a*b", "aaaaaaaaaaaaaa"));
}

static void test_text(void) {
    puts("textutil");
    const char *t = "one\ntwo\nthree\nfour\n";   /* 19 bytes */
    size_t L = strlen(t);
    CHECK(text_head_len(t, L, 0) == 0 && text_head_len(t, L, 1) == 4 && text_head_len(t, L, 2) == 8 && text_head_len(t, L, 4) == L && text_head_len(t, L, 99) == L);
    CHECK(text_head_len("", 0, 5) == 0 && text_head_len("abc", 3, 1) == 3);                       /* no trailing newline */
    CHECK(text_tail_start(t, L, 0) == L && text_tail_start(t, L, 1) == 14 && text_tail_start(t, L, 2) == 8 && text_tail_start(t, L, 4) == 0 && text_tail_start(t, L, 99) == 0);
    CHECK(text_tail_start("a\nb", 3, 1) == 2 && text_tail_start("a\nb", 3, 2) == 0 && text_tail_start("", 0, 3) == 0 && text_tail_start("\n", 1, 1) == 0);
    CHECK(text_tail_start("a\n\n", 3, 1) == 2);                                                  /* last line is empty */
    size_t l, w, b;
    text_counts(t, L, &l, &w, &b); CHECK(l == 4 && w == 4 && b == L);
    text_counts("", 0, &l, &w, &b); CHECK(l == 0 && w == 0 && b == 0);
    text_counts("  a  b\t\tc\n\n", 11, &l, &w, &b); CHECK(l == 2 && w == 3);
    text_counts("no newline", 10, &l, &w, &b); CHECK(l == 0 && w == 2);
    text_counts("x", 1, NULL, NULL, NULL);                                                       /* all outputs optional */
    CHECK(text_is_text("hello\tworld\r\n", 13) && !text_is_text("a\x01" "b", 3) && !text_is_text("\xff", 1) && text_is_text("", 0));
    CHECK(text_contains("Hello World", 11, "World", false) && !text_contains("Hello World", 11, "world", false) && text_contains("Hello World", 11, "world", true));
    CHECK(text_contains("abc", 3, "", false) && !text_contains("abc", 3, "abcd", false) && text_contains("abc", 3, "abc", false) && !text_contains("", 0, "a", false));
    CHECK(text_contains("abcabd", 6, "abd", false) && !text_contains("abc", 2, "abc", false));    /* length limits the search */
    CHECK(!text_contains(NULL, 0, "a", false) && !text_contains("a", 1, NULL, false));
}

static bool owner_flag;
static bool is_owner(void) { return owner_flag; }

static void test_capture(void) {
    char buf[8]; bool ov = true;
    puts("capture");
    CHECK(!capture_active() && !capture_write('x'));                    /* nothing captured when inactive */
    capture_begin(buf, sizeof buf, NULL);
    CHECK(capture_active() && capture_write('a') && capture_write('b'));
    CHECK(capture_end(&ov) == 2 && !ov && memcmp(buf, "ab", 2) == 0 && !capture_active());
    CHECK(!capture_write('z'));                                          /* ended */
    capture_begin(buf, sizeof buf, NULL);
    for (int i = 0; i < 8; i++) CHECK(capture_write('0' + (char)i));
    CHECK(capture_write('X') && capture_write('Y'));                     /* overflow is swallowed, not displayed */
    CHECK(capture_end(&ov) == 8 && ov && memcmp(buf, "01234567", 8) == 0);
    owner_flag = false; capture_begin(buf, sizeof buf, is_owner);
    CHECK(!capture_write('n'));                                          /* another task: not captured */
    owner_flag = true; CHECK(capture_write('y'));
    CHECK(capture_end(NULL) == 1 && buf[0] == 'y');
    capture_begin(NULL, 10, NULL); CHECK(!capture_active());             /* invalid buffers do not start a capture */
    capture_begin(buf, 0, NULL);   CHECK(!capture_active() && !capture_write('q'));
    capture_begin(buf, sizeof buf, NULL); capture_begin(buf, sizeof buf, NULL);
    CHECK(capture_end(NULL) == 0);                                       /* restarting resets the length */
}

int main(void) {
    test_glob(); test_text(); test_capture();
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
