#ifndef COMMAND_H
#define COMMAND_H
/* MiniOS command registry (Phase 5).
 *
 * One table describes every command: id, name, description, category, level,
 * subsystem and handler. The table itself is plain data (commands/command_table.c,
 * generated from scripts/commands.txt); handlers are attached at boot with
 * command_bind(). A command with no handler is a STUB: it is registered and
 * documented but prints a clear "not implemented yet" message when run.
 * This file has no kernel dependencies, so it is unit-tested on the host. */
#include "include/types.h"

#define COMMAND_EXPECTED 300

typedef enum {
    CAT_SHELL, CAT_FILE, CAT_DIR, CAT_PROCESS, CAT_MEMORY, CAT_USER, CAT_SECURITY,
    CAT_DISK, CAT_SYSTEM, CAT_TEXT, CAT_NETWORK, CAT_DEBUG, CAT_OTHER, CAT_COUNT
} cmd_category_t;

#define LEVEL_L1 1   /* real, changes real kernel state                      */
#define LEVEL_L2 2   /* real, built on existing subsystems                   */
#define LEVEL_L3 3   /* SIMULATED: never claims to touch real hardware       */

typedef int (*cmd_handler_t)(int argc, const char **argv);

typedef struct {
    uint16_t       id;        /* 1..300, equals position+1, stable            */
    const char    *name;      /* lowercase letters/digits                     */
    const char    *desc;
    const char    *subsys;    /* "FS", "SEC+FS", ...                          */
    uint8_t        category;  /* cmd_category_t                               */
    uint8_t        level;     /* LEVEL_L1..L3                                 */
    cmd_handler_t  handler;   /* NULL = stub (registered, not implemented)    */
} command_t;

/* the table (commands/command_table.c) */
extern command_t    command_table[];
extern const size_t command_table_len;

/* ---- lookup ---- */
size_t           command_count(void);
const command_t *command_at(size_t index);                /* NULL if out of range */
const command_t *command_find(const char *name);          /* NULL if not found    */

/* ---- binding handlers ---- */
#define CMD_BIND_OK        0
#define CMD_BIND_UNKNOWN (-1)   /* no such command name in the table */
#define CMD_BIND_NULLFN  (-2)
int command_bind(const char *name, cmd_handler_t fn);

/* ---- statistics ---- */
size_t command_count_level(int level);
size_t command_count_implemented(void);
size_t command_count_category(cmd_category_t cat, size_t *implemented_out);

/* ---- names ---- */
const char *command_category_name(cmd_category_t cat);    /* "file", "network", ... */
int         command_category_from_name(const char *s);    /* accepts aliases; -1 if unknown */
const char *command_level_name(int level);                /* "L1".."L3" */
const char *command_level_meaning(int level);

/* Earliest roadmap phase in which every subsystem of this command exists. */
int command_planned_phase(const command_t *c);
const char *command_planned_phase_what(const command_t *c);   /* e.g. "MiniFS" */

/* ---- validation (run at boot and in host tests) ---- */
#define CMDV_OK               0
#define CMDV_COUNT          (-1)   /* table size != COMMAND_EXPECTED         */
#define CMDV_EMPTY_NAME     (-2)
#define CMDV_BAD_NAME       (-3)   /* uppercase, space or punctuation        */
#define CMDV_DUPLICATE      (-4)
#define CMDV_BAD_LEVEL      (-5)
#define CMDV_BAD_CATEGORY   (-6)
#define CMDV_BAD_ID         (-7)
#define CMDV_NO_DESC        (-8)
#define CMDV_CATEGORY_SPLIT (-9)   /* category sizes differ from 25/35/20/... */
#define CMDV_NO_SUBSYS     (-10)
int         command_validate(size_t *bad_index);          /* 0 or CMDV_*; index of offender */
const char *command_validate_error(int code);
#endif
