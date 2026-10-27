/* Shell tokenizer. Pure logic: no kernel dependencies, so it is unit-tested
 * on the host (see tests/test_shell.c). */
#include "include/shell.h"

static bool is_space(char c) { return c == ' ' || c == '\t'; }

int tokenize(const char *line, char *buf, size_t bufsz, token_t *out, int max) {
    size_t w = 0;
    int n = 0;
    const char *r = line;

    if (!line || !buf || !out || bufsz == 0) return TOK_ERR_BUFFER;

    while (*r) {
        while (is_space(*r)) r++;
        if (!*r) break;
        if (n >= max) return TOK_ERR_TOO_MANY;

        if (*r == '>') {                          /* redirection operator */
            bool append = (r[1] == '>');
            out[n].type = append ? TOK_REDIR_APPEND : TOK_REDIR_OUT;
            out[n].text = append ? ">>" : ">";
            r += append ? 2 : 1;
            n++;
            continue;
        }

        if (*r == '|') {                          /* pipe operator */
            out[n].type = TOK_PIPE; out[n].text = "|";
            r++; n++;
            continue;
        }

        out[n].type = TOK_WORD;                   /* a word, possibly quoted */
        out[n].text = &buf[w];
        while (*r && !is_space(*r) && *r != '>' && *r != '|') {
            if (*r == '"' || *r == '\'') {
                char quote = *r++;
                while (*r && *r != quote) {
                    if (w + 1 >= bufsz) return TOK_ERR_BUFFER;
                    buf[w++] = *r++;
                }
                if (!*r) return TOK_ERR_UNTERMINATED;
                r++;                              /* skip closing quote */
            } else {
                if (w + 1 >= bufsz) return TOK_ERR_BUFFER;
                buf[w++] = *r++;
            }
        }
        if (w + 1 > bufsz) return TOK_ERR_BUFFER;
        buf[w++] = '\0';
        n++;
    }
    return n;
}
