#ifndef SHELLVARS_H
#define SHELLVARS_H
/* Shell variables and aliases (state of the shell itself, not of the kernel).
 * REAL:  fixed tables, $NAME / ${NAME} / $? expansion (not inside single quotes), alias substitution of the first word.
 * NOT:   functions, arrays, command substitution, arithmetic expansion. (if/for/while live in shell/lineexec.c since Phase 15.) */
#include "include/types.h"

#define VAR_MAX          16
#define VAR_NAME_MAX     15
#define VAR_VALUE_MAX    63
#define ALIAS_MAX        16

#define SV_OK              0
#define SV_ERR_NAME      (-1)   /* empty, too long, or not [A-Za-z_][A-Za-z0-9_]* */
#define SV_ERR_VALUE     (-2)   /* value longer than VAR_VALUE_MAX */
#define SV_ERR_FULL      (-3)   /* table full */
#define SV_ERR_NOT_FOUND (-4)
#define SV_ERR_SPACE     (-5)   /* expansion does not fit in the output buffer */
#define SV_ERR_RECURSION (-6)   /* alias expands to itself */

void        sv_reset(void);
void        sv_set_status(int status);                                        /* value of $? */
bool        sv_name_valid(const char *s);
int         var_set(const char *name, const char *value, bool exported);   /* keeps the exported flag of an existing variable if `exported` is false */
int         var_unset(const char *name);
const char *var_get(const char *name);                                       /* NULL if unset */
int         var_export(const char *name);
size_t      var_count(void);
bool        var_at(size_t index, const char **name, const char **value, bool *exported);   /* sorted by name */

int         alias_set(const char *name, const char *value);
int         alias_remove(const char *name);
const char *alias_get(const char *name);
size_t      alias_count(void);
bool        alias_at(size_t index, const char **name, const char **value);                 /* sorted by name */

/* Replaces $NAME, ${NAME} and $? in `in`. A '$' not followed by a name is kept. Single-quoted text is copied unchanged. */
int         sv_expand_vars(const char *in, char *out, size_t cap);
/* If the first word of `in` is an alias, replaces it by its value (once). *changed tells whether it did. */
int         sv_expand_alias(const char *in, char *out, size_t cap, bool *changed);
#endif
