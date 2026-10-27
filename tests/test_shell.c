/* Host-side unit tests for tokenizer, parser and history (run: make test).
 * These compile the REAL kernel sources with the host gcc. */
#include <stdio.h>
#include <string.h>
#include "include/shell.h"

static int run, failed;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; \
    printf("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static int parse(const char *line, command_line_t *cl) {
    static char buf[SHELL_LINE_MAX * 2];
    return parse_command_line(line, buf, sizeof(buf), cl);
}

static void test_tokenizer(void) {
    char buf[64]; token_t t[8];
    puts("tokenizer");
    CHECK(tokenize("", buf, sizeof buf, t, 8) == 0);
    CHECK(tokenize("   \t ", buf, sizeof buf, t, 8) == 0);
    CHECK(tokenize("ls -l  /tmp", buf, sizeof buf, t, 8) == 3);
    CHECK(strcmp(t[2].text, "/tmp") == 0);
    CHECK(tokenize("echo \"a  b\"", buf, sizeof buf, t, 8) == 2);
    CHECK(strcmp(t[1].text, "a  b") == 0);                 /* spaces kept inside quotes */
    CHECK(tokenize("echo 'x\"y'", buf, sizeof buf, t, 8) == 2);
    CHECK(strcmp(t[1].text, "x\"y") == 0);                 /* other quote kind is literal */
    CHECK(tokenize("echo \"unterminated", buf, sizeof buf, t, 8) == TOK_ERR_UNTERMINATED);
    CHECK(tokenize("echo \"a\">f", buf, sizeof buf, t, 8) == 4);
    CHECK(t[2].type == TOK_REDIR_OUT);
    CHECK(tokenize("echo hi>>f", buf, sizeof buf, t, 8) == 4);
    CHECK(t[2].type == TOK_REDIR_APPEND);
    CHECK(tokenize("echo \">\"", buf, sizeof buf, t, 8) == 2);   /* quoted > is a word */
    CHECK(t[1].type == TOK_WORD);
    CHECK(tokenize("a b c d e", buf, sizeof buf, t, 4) == TOK_ERR_TOO_MANY);   /* boundary */
    CHECK(tokenize("a b c d", buf, sizeof buf, t, 4) == 4);                    /* exactly max */
    CHECK(tokenize("abcdefghij", buf, 5, t, 8) == TOK_ERR_BUFFER);             /* tiny buffer */
    CHECK(tokenize(NULL, buf, sizeof buf, t, 8) == TOK_ERR_BUFFER);
    CHECK(tokenize("ab\"cd ef\"gh", buf, sizeof buf, t, 8) == 1);              /* joined quoted word */
    CHECK(strcmp(t[0].text, "abcd efgh") == 0);
}

static void test_parser(void) {
    command_line_t cl;
    puts("parser");
    CHECK(parse("", &cl) == PARSE_EMPTY);
    CHECK(parse("   ", &cl) == PARSE_EMPTY);
    CHECK(parse("echo hello world", &cl) == PARSE_OK);
    CHECK(cl.argc == 3 && strcmp(cl.argv[0], "echo") == 0 && cl.argv[3] == NULL);
    CHECK(cl.redir == REDIR_NONE);
    CHECK(parse("echo \"Hello MiniOS\" > hello.txt", &cl) == PARSE_OK);
    CHECK(cl.argc == 2 && cl.redir == REDIR_TRUNCATE && strcmp(cl.redir_path, "hello.txt") == 0);
    CHECK(parse("echo hi >> log", &cl) == PARSE_OK && cl.redir == REDIR_APPEND);
    CHECK(parse("echo hi >", &cl) == PARSE_ERR_REDIR_TARGET);
    CHECK(parse("echo hi > >", &cl) == PARSE_ERR_REDIR_TARGET);
    CHECK(parse("echo hi > a > b", &cl) == PARSE_ERR_REDIR_TWICE);
    CHECK(parse("> file", &cl) == PARSE_ERR_NO_COMMAND);
    CHECK(parse("echo \"oops", &cl) == PARSE_ERR_QUOTE);
    CHECK(parse("> a b", &cl) == PARSE_OK && cl.argc == 1 && strcmp(cl.argv[0], "b") == 0);   /* like real shells */
    CHECK(parse("echo > f extra", &cl) == PARSE_OK && cl.argc == 2);   /* args after target kept */
    /* 16 args ok, 17 rejected */
    CHECK(parse("c 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15", &cl) == PARSE_OK && cl.argc == 16);
    CHECK(parse("c 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16", &cl) == PARSE_ERR_TOO_MANY);
    CHECK(parse_command_line("x", NULL, 0, &cl) == PARSE_ERR_BUFFER);
    CHECK(strlen(parse_error_string(PARSE_ERR_QUOTE)) > 0);
}

static void test_history(void) {
    puts("history");
    history_init();
    CHECK(history_count() == 0 && history_get(0) == NULL);
    history_add(""); CHECK(history_count() == 0);                /* blank ignored */
    history_add("ls"); history_add("ls"); CHECK(history_count() == 1);   /* dup ignored */
    history_add("pwd"); history_add("ls");
    CHECK(history_count() == 3 && strcmp(history_get(2), "ls") == 0);
    CHECK(history_get(-1) == NULL && history_get(3) == NULL);
    history_init();
    char b[16];
    for (int i = 0; i < HISTORY_SIZE + 5; i++) { snprintf(b, sizeof b, "cmd%d", i); history_add(b); }
    CHECK(history_count() == HISTORY_SIZE);                      /* ring is full, not larger */
    CHECK(strcmp(history_get(0), "cmd5") == 0);                  /* oldest 5 were dropped */
    CHECK(strcmp(history_get(HISTORY_SIZE - 1), "cmd20") == 0);
    char longline[SHELL_LINE_MAX + 50]; memset(longline, 'x', sizeof longline - 1);
    longline[sizeof longline - 1] = 0; history_init(); history_add(longline);
    CHECK(strlen(history_get(0)) == SHELL_LINE_MAX - 1);         /* truncated safely */
}

static void test_pipes(void) {
    puts("pipes: tokenizer");
    char buf[256]; token_t t[SHELL_MAX_TOKENS];
    CHECK(tokenize("a|b", buf, sizeof buf, t, 8) == 3 && t[0].type == TOK_WORD && t[1].type == TOK_PIPE && t[2].type == TOK_WORD && !strcmp(t[2].text, "b"));
    CHECK(tokenize("a | b | c", buf, sizeof buf, t, 8) == 5 && t[1].type == TOK_PIPE && t[3].type == TOK_PIPE);
    CHECK(tokenize("echo \"a | b\" 'c|d'", buf, sizeof buf, t, 8) == 3 && !strcmp(t[1].text, "a | b") && !strcmp(t[2].text, "c|d"));   /* quoted pipes are text */
    CHECK(tokenize("a||b", buf, sizeof buf, t, 8) == 4 && t[1].type == TOK_PIPE && t[2].type == TOK_PIPE);
    CHECK(tokenize("a|>b", buf, sizeof buf, t, 8) == 4 && t[1].type == TOK_PIPE && t[2].type == TOK_REDIR_OUT);
    CHECK(tokenize("|", buf, sizeof buf, t, 8) == 1 && t[0].type == TOK_PIPE && tokenize("a |", buf, sizeof buf, t, 8) == 2);
    CHECK(tokenize("a | b", buf, sizeof buf, t, 2) == TOK_ERR_TOO_MANY);

    puts("pipes: parser");
    pipeline_t p; char b[SHELL_LINE_MAX * 2];
    CHECK(parse_pipeline("ls", b, sizeof b, &p) == PARSE_OK && p.nstages == 1 && p.stage[0].argc == 1);
    CHECK(parse_pipeline("cat f | sort -r | uniq -c", b, sizeof b, &p) == PARSE_OK && p.nstages == 3 && p.stage[0].argc == 2 && !strcmp(p.stage[1].argv[0], "sort") && p.stage[1].argc == 2 && !strcmp(p.stage[2].argv[1], "-c"));
    CHECK(p.stage[0].redir == REDIR_NONE && p.stage[2].redir == REDIR_NONE);
    CHECK(parse_pipeline("a | b | c | d", b, sizeof b, &p) == PARSE_OK && p.nstages == 4);
    CHECK(parse_pipeline("a | b | c | d | e", b, sizeof b, &p) == PARSE_ERR_TOO_MANY_STAGES);
    CHECK(parse_pipeline("cat f | sort > out", b, sizeof b, &p) == PARSE_OK && p.nstages == 2 && p.stage[1].redir == REDIR_TRUNCATE && !strcmp(p.stage[1].redir_path, "out") && p.stage[0].redir == REDIR_NONE);
    CHECK(parse_pipeline("cat f | sort >> out", b, sizeof b, &p) == PARSE_OK && p.stage[1].redir == REDIR_APPEND);
    CHECK(parse_pipeline("cat f > x | sort", b, sizeof b, &p) == PARSE_ERR_PIPE_REDIR);
    CHECK(parse_pipeline("a |", b, sizeof b, &p) == PARSE_ERR_EMPTY_STAGE && parse_pipeline("| a", b, sizeof b, &p) == PARSE_ERR_EMPTY_STAGE && parse_pipeline("a || b", b, sizeof b, &p) == PARSE_ERR_EMPTY_STAGE && parse_pipeline("|", b, sizeof b, &p) == PARSE_ERR_EMPTY_STAGE);
    CHECK(parse_pipeline("a | > f", b, sizeof b, &p) == PARSE_ERR_EMPTY_STAGE);          /* a redirection is not a command */
    CHECK(parse_pipeline("a | b >", b, sizeof b, &p) == PARSE_ERR_REDIR_TARGET && parse_pipeline("a | b > f > g", b, sizeof b, &p) == PARSE_ERR_REDIR_TWICE);
    CHECK(parse_pipeline("a | 'b", b, sizeof b, &p) == PARSE_ERR_QUOTE && parse_pipeline("", b, sizeof b, &p) == PARSE_EMPTY && parse_pipeline("   ", b, sizeof b, &p) == PARSE_EMPTY);
    CHECK(parse_pipeline("> f", b, sizeof b, &p) == PARSE_ERR_NO_COMMAND);                     /* unchanged from before pipes existed */
    CHECK(parse_pipeline("a | b", NULL, 0, &p) == PARSE_ERR_BUFFER && parse_pipeline("a", b, sizeof b, NULL) == PARSE_ERR_BUFFER);
    char tiny[6]; CHECK(parse_pipeline("a | bbbbbbbbbbbbbbbbbbbbbb", tiny, sizeof tiny, &p) == PARSE_ERR_BUFFER);
    CHECK(parse_pipeline("echo \"a | b\" | cat", b, sizeof b, &p) == PARSE_OK && p.nstages == 2 && !strcmp(p.stage[0].argv[1], "a | b"));
    for (int i = 0; i < 20; i++) CHECK(parse_pipeline("a | b", b, sizeof b, &p) == PARSE_OK && p.stage[0].argv[p.stage[0].argc] == NULL && p.stage[1].argv[p.stage[1].argc] == NULL);

    puts("pipes: parse_command_line (one command) refuses a pipeline");
    command_line_t cl;
    CHECK(parse_command_line("a | b", b, sizeof b, &cl) == PARSE_ERR_PIPE && parse_command_line("ls -l > f", b, sizeof b, &cl) == PARSE_OK && cl.redir == REDIR_TRUNCATE);
    CHECK(strlen(parse_error_string(PARSE_ERR_EMPTY_STAGE)) > 5 && strlen(parse_error_string(PARSE_ERR_PIPE_REDIR)) > 5 && strlen(parse_error_string(PARSE_ERR_TOO_MANY_STAGES)) > 5 && strlen(parse_error_string(PARSE_ERR_PIPE)) > 5);
}

static void test_lists(void) {
    puts("command lists: splitting");
    segment_t g[LIST_MAX_SEGMENTS];
    #define SEG(i, str) (g[i].len == strlen(str) && !strncmp(g[i].text, str, g[i].len))
    CHECK(split_command_list("echo a", g, 8) == 1 && SEG(0, "echo a") && g[0].conn == CONN_FIRST);
    CHECK(split_command_list("a ; b", g, 8) == 2 && SEG(0, "a ") && SEG(1, " b") && g[0].conn == CONN_FIRST && g[1].conn == CONN_SEMI);
    CHECK(split_command_list("a;b;c", g, 8) == 3 && SEG(0, "a") && SEG(1, "b") && SEG(2, "c") && g[2].conn == CONN_SEMI);
    CHECK(split_command_list("a && b", g, 8) == 2 && g[1].conn == CONN_AND && SEG(1, " b"));
    CHECK(split_command_list("a || b", g, 8) == 2 && g[1].conn == CONN_OR && SEG(1, " b"));
    CHECK(split_command_list("a&&b||c;d", g, 8) == 4 && g[1].conn == CONN_AND && g[2].conn == CONN_OR && g[3].conn == CONN_SEMI && SEG(3, "d"));
    CHECK(split_command_list("a | b || c", g, 8) == 2 && SEG(0, "a | b ") && g[1].conn == CONN_OR);          /* one | is a pipe, two are OR */
    CHECK(split_command_list("a | b > f ; c", g, 8) == 2 && SEG(0, "a | b > f "));
    CHECK(split_command_list("echo \"a;b\" ; echo 'c && d'", g, 8) == 2 && SEG(0, "echo \"a;b\" ") && SEG(1, " echo 'c && d'"));    /* quotes hide the operators */
    CHECK(split_command_list("echo \"a || b\"", g, 8) == 1 && split_command_list("echo 'x & y'", g, 8) == 1);
    CHECK(split_command_list("echo hi ;", g, 8) == 1 && split_command_list("echo hi ;   ", g, 8) == 1 && split_command_list("a ; b ;", g, 8) == 2);   /* a trailing ; is fine */
    CHECK(split_command_list("", g, 8) == 0 && split_command_list("   \t ", g, 8) == 0);
    CHECK(split_command_list("\ta\t;\tb\t", g, 8) == 2);
    puts("command lists: errors");
    CHECK(split_command_list(";", g, 8) == PARSE_ERR_LIST_EMPTY && split_command_list("; a", g, 8) == PARSE_ERR_LIST_EMPTY && split_command_list("a ;; b", g, 8) == PARSE_ERR_LIST_EMPTY);
    CHECK(split_command_list("&& a", g, 8) == PARSE_ERR_LIST_EMPTY && split_command_list("a &&", g, 8) == PARSE_ERR_LIST_EMPTY && split_command_list("a ||", g, 8) == PARSE_ERR_LIST_EMPTY && split_command_list("|| a", g, 8) == PARSE_ERR_LIST_EMPTY);
    CHECK(split_command_list("a && && b", g, 8) == PARSE_ERR_LIST_EMPTY && split_command_list("a ; && b", g, 8) == PARSE_ERR_LIST_EMPTY && split_command_list("a && ;", g, 8) == PARSE_ERR_LIST_EMPTY);
    CHECK(split_command_list("a & b", g, 8) == PARSE_ERR_BACKGROUND && split_command_list("a &", g, 8) == PARSE_ERR_BACKGROUND && split_command_list("&", g, 8) == PARSE_ERR_BACKGROUND);
    CHECK(split_command_list("a ; echo \"b", g, 8) == PARSE_ERR_QUOTE && split_command_list("'", g, 8) == PARSE_ERR_QUOTE);
    CHECK(split_command_list("1;2;3;4;5;6;7;8", g, 8) == 8 && split_command_list("1;2;3;4;5;6;7;8;9", g, 8) == PARSE_ERR_LIST_TOO_LONG);          /* the limit is the caller's: the shell uses 16 */
    CHECK(split_command_list("1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16", g, LIST_MAX_SEGMENTS) == 16 && split_command_list("1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17", g, LIST_MAX_SEGMENTS) == PARSE_ERR_LIST_TOO_LONG);
    CHECK(split_command_list(NULL, g, 8) == PARSE_ERR_BUFFER && split_command_list("a", NULL, 8) == PARSE_ERR_BUFFER && split_command_list("a", g, 0) == PARSE_ERR_BUFFER);
    CHECK(strlen(parse_error_string(PARSE_ERR_LIST_EMPTY)) > 5 && strstr(parse_error_string(PARSE_ERR_BACKGROUND), "spawn") && strstr(parse_error_string(PARSE_ERR_LIST_TOO_LONG), "16") && strlen(parse_error_string(PARSE_ERR_KW_TEXT)) > 5);
    for (int i = 0; i < 20; i++) CHECK(split_command_list("a ; b && c || d", g, 8) == 4 && g[0].conn == CONN_FIRST);       /* repeatable: no hidden state */
}

/* "IF,N,THEN,N,FI": the keyword kinds of a line after expand_keywords */
static const char *kinds(const char *line, char *out) {
    segment_t g[LIST_MAX_SEGMENTS]; xseg_t x[XSEG_MAX];
    static const char *const nm[] = { "N", "IF", "THEN", "ELIF", "ELSE", "FI", "WHILE", "FOR", "DO", "DONE", "BREAK", "CONTINUE" };
    int n = split_command_list(line, g, LIST_MAX_SEGMENTS);
    if (n < 0) { snprintf(out, 80, "split:%d", n); return out; }
    int m = expand_keywords(g, n, x, XSEG_MAX);
    if (m < 0) { snprintf(out, 80, "expand:%d", m); return out; }
    out[0] = 0;
    for (int i = 0; i < m; i++) { if (i) strcat(out, ","); strcat(out, nm[x[i].kw]); }
    return out;
}
static void test_keywords(void) {
    puts("control flow: keywords");
    char k[160];
    CHECK(!strcmp(kinds("echo hi", k), "N") && !strcmp(kinds("", k), ""));
    CHECK(!strcmp(kinds("if a ; then b ; fi", k), "IF,N,THEN,N,FI"));
    CHECK(!strcmp(kinds("if a ; then b ; else c ; fi", k), "IF,N,THEN,N,ELSE,N,FI"));
    CHECK(!strcmp(kinds("if a ; then b ; elif c ; then d ; else e ; fi", k), "IF,N,THEN,N,ELIF,N,THEN,N,ELSE,N,FI"));
    CHECK(!strcmp(kinds("while a ; do b ; done", k), "WHILE,N,DO,N,DONE"));
    CHECK(!strcmp(kinds("for x in 1 2 ; do echo $x ; done", k), "FOR,DO,N,DONE"));
    CHECK(!strcmp(kinds("for x in 1 2 ; do echo $x ; break ; continue ; done", k), "FOR,DO,N,BREAK,CONTINUE,DONE"));
    CHECK(!strcmp(kinds("if a && b ; then c || d ; fi", k), "IF,N,N,THEN,N,N,FI"));
    CHECK(!strcmp(kinds("if a ; then ; b ; fi", k), "IF,N,THEN,N,FI"));                         /* "then" alone, command on the next segment */
    CHECK(!strcmp(kinds("while a ; do if b ; then c ; fi ; done", k), "WHILE,N,DO,IF,N,THEN,N,FI,DONE"));
    CHECK(!strcmp(kinds("if a ; then if b ; then c ; fi ; fi", k), "IF,N,THEN,IF,N,THEN,N,FI,FI"));          /* "then if b" is split into two entries */
    puts("keywords only count as the FIRST word");
    CHECK(!strcmp(kinds("echo if then fi", k), "N") && !strcmp(kinds("echo done ; echo for", k), "N,N"));
    CHECK(!strcmp(kinds("'if' x", k), "N") && !strcmp(kinds("\"while\" x", k), "N") && !strcmp(kinds("iffy", k), "N") && !strcmp(kinds("format", k), "N") && !strcmp(kinds("donut", k), "N") && !strcmp(kinds("breaker", k), "N"));
    CHECK(!strcmp(kinds("if", k), "IF") && !strcmp(kinds("   if   ", k), "IF") && !strcmp(kinds("\tthen", k), "THEN"));
    puts("keywords that take no text");
    CHECK(!strcmp(kinds("fi echo", k), "expand:-14") && !strcmp(kinds("done | sort", k), "expand:-14") && !strcmp(kinds("break now", k), "expand:-14") && !strcmp(kinds("continue 2", k), "expand:-14"));
    CHECK(!strcmp(kinds("fi", k), "FI") && !strcmp(kinds("done", k), "DONE") && !strcmp(kinds("break", k), "BREAK") && !strcmp(kinds("continue", k), "CONTINUE"));
    CHECK(!strcmp(kinds("echo a ; fi", k), "N,FI"));
    puts("the text that follows a keyword");
    segment_t g[LIST_MAX_SEGMENTS]; xseg_t x[XSEG_MAX]; int n = split_command_list("if test -f x ; then echo yes ; fi", g, LIST_MAX_SEGMENTS);
    int m = expand_keywords(g, n, x, XSEG_MAX);
    CHECK(m == 5 && x[0].kw == KW_IF && x[0].len == 0 && !strncmp(x[1].text, "test -f x", 9) && x[1].conn == CONN_FIRST && !strncmp(x[3].text, "echo yes", 8) && x[2].conn == CONN_SEMI);
    n = split_command_list("for f in a b c ; do echo $f ; done", g, LIST_MAX_SEGMENTS); m = expand_keywords(g, n, x, XSEG_MAX);
    CHECK(x[0].kw == KW_FOR && !strncmp(x[0].text, "f in a b c", 10));
    n = split_command_list("for ; do echo x ; done", g, LIST_MAX_SEGMENTS); m = expand_keywords(g, n, x, XSEG_MAX); CHECK(x[0].kw == KW_FOR && x[0].len == 0);
    n = split_command_list("if a && b ; then c ; fi", g, LIST_MAX_SEGMENTS); m = expand_keywords(g, n, x, XSEG_MAX); CHECK(x[1].conn == CONN_FIRST && x[2].conn == CONN_AND);
    puts("limit");
    n = split_command_list("then if a ; then if a ; then if a ; then if a ; then if a ; then if a ; then if a ; then if a ; then if a ; then if a ; then if a ; then if a ; then if a ; then if a", g, LIST_MAX_SEGMENTS);
    CHECK(n == 14 && expand_keywords(g, n, x, XSEG_MAX) == PARSE_ERR_LIST_TOO_LONG);                  /* every "then if a" becomes three entries: 42 > 40 */
    CHECK(expand_keywords(g, 0, x, XSEG_MAX) == 0);

    puts("control flow: an unfinished block asks for another line");
    CHECK(command_list_incomplete("if a ; then b") && command_list_incomplete("if a") && command_list_incomplete("for x in 1 2") && command_list_incomplete("for x in 1 2 ; do") && command_list_incomplete("while a ; do b"));
    CHECK(command_list_incomplete("if a ; then if b ; then c ; fi") && command_list_incomplete("while a ; do for x in 1 ; do b ; done") && command_list_incomplete("if a ; then b ; else c"));
    CHECK(!command_list_incomplete("if a ; then b ; fi") && !command_list_incomplete("for x in 1 ; do b ; done") && !command_list_incomplete("echo hi") && !command_list_incomplete(""));
    CHECK(!command_list_incomplete("fi") && !command_list_incomplete("done") && !command_list_incomplete("echo \"unterminated") && !command_list_incomplete("a &&") && !command_list_incomplete("a & b") && !command_list_incomplete("fi echo"));
    CHECK(!command_list_incomplete("if a ; then b ; fi ; if c") == false && !command_list_incomplete("echo if") && !command_list_incomplete("'if' x"));
}

int main(void) {
    test_tokenizer(); test_parser(); test_history(); test_pipes(); test_lists(); test_keywords();
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
