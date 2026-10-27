#ifndef COMPLETE_H
#define COMPLETE_H
/* Tab completion (Phase 16). complete_word looks at the word under the cursor and finds what could follow:
 *   - first word of a command (after ; | & && || or then/do/else/if/elif/while/sudo): command names and shell keywords
 *   - a word starting with $ : variable names
 *   - anything else (or a word containing '/', or after > and <): file and directory names
 * Pure logic: the three lists come from callbacks, so the host tests use fake lists and the shell uses the real command table, variables
 * and MiniFS (shell/compenv.c). Names that contain spaces, quotes or shell operators are never offered (they would break the line). */
#include "include/types.h"

#define COMP_MAX      64
#define COMP_NAME_MAX 48
#define COMP_TEXT_MAX 128

typedef void (*comp_emit_fn)(const char *name, bool is_dir, void *ctx);
typedef struct {
    void (*commands)(comp_emit_fn emit, void *ctx);
    void (*variables)(comp_emit_fn emit, void *ctx);
    void (*entries)(const char *dir, comp_emit_fn emit, void *ctx);   /* dir "" = the working directory, otherwise the text as typed, ending in '/' */
} comp_env_t;

typedef enum { COMP_NONE, COMP_UNIQUE, COMP_PARTIAL, COMP_LIST } comp_kind_t;
typedef struct {
    comp_kind_t kind;
    size_t      word_start;            /* the word begins here in the line; it ends at the cursor */
    char        text[COMP_TEXT_MAX];   /* what the word should become (UNIQUE: the whole name; PARTIAL: the common start) */
    bool        append_space;          /* UNIQUE and not a directory or variable: a space may follow */
    char        cand[COMP_MAX][COMP_NAME_MAX];   /* the names to show (the part after the directory), sorted */
    bool        cand_dir[COMP_MAX];
    size_t      ncand, total;          /* total can exceed COMP_MAX */
} comp_result_t;

const comp_env_t *complete_env_system(void);      /* shell/compenv.c: the real command table, variables and MiniFS */
comp_kind_t complete_word(const char *line, size_t cursor, const comp_env_t *env, comp_result_t *out);
bool        comp_name_safe(const char *name);       /* false for names that cannot be typed without quoting */
#endif
