/* Registry logic: lookup, binding, statistics, validation. Pure C: no kernel
 * calls, no libc, so the host unit tests run the exact same code as the kernel. */
#include "include/command.h"

static bool str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static size_t str_len(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

size_t command_count(void) { return command_table_len; }

const command_t *command_at(size_t i) { return i < command_table_len ? &command_table[i] : NULL; }

const command_t *command_find(const char *name) {
    if (!name || !*name) return NULL;
    for (size_t i = 0; i < command_table_len; i++)
        if (str_eq(command_table[i].name, name)) return &command_table[i];
    return NULL;
}

int command_bind(const char *name, cmd_handler_t fn) {
    if (!fn) return CMD_BIND_NULLFN;
    const command_t *c = command_find(name);
    if (!c) return CMD_BIND_UNKNOWN;
    command_table[c->id - 1].handler = fn;
    return CMD_BIND_OK;
}

/* ---------- statistics ---------- */
size_t command_count_level(int level) {
    size_t n = 0;
    for (size_t i = 0; i < command_table_len; i++) if (command_table[i].level == level) n++;
    return n;
}
size_t command_count_implemented(void) {
    size_t n = 0;
    for (size_t i = 0; i < command_table_len; i++) if (command_table[i].handler) n++;
    return n;
}
size_t command_count_category(cmd_category_t cat, size_t *impl) {
    size_t n = 0, k = 0;
    for (size_t i = 0; i < command_table_len; i++)
        if (command_table[i].category == cat) { n++; if (command_table[i].handler) k++; }
    if (impl) *impl = k;
    return n;
}

/* ---------- names ---------- */
static const char *const cat_names[CAT_COUNT] = {
    "shell", "file", "directory", "process", "memory", "user", "security",
    "disk", "system", "text", "network", "debug", "other"
};
const char *command_category_name(cmd_category_t c) { return (unsigned)c < CAT_COUNT ? cat_names[c] : "?"; }

int command_category_from_name(const char *s) {
    static const struct { const char *alias; int cat; } extra[] = {
        { "dir", CAT_DIR }, { "proc", CAT_PROCESS }, { "mem", CAT_MEMORY },
        { "perm", CAT_SECURITY }, { "permission", CAT_SECURITY }, { "sec", CAT_SECURITY },
        { "sys", CAT_SYSTEM }, { "net", CAT_NETWORK }, { "log", CAT_DEBUG },
    };
    if (!s || !*s) return -1;
    for (int i = 0; i < CAT_COUNT; i++) if (str_eq(s, cat_names[i])) return i;
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) if (str_eq(s, extra[i].alias)) return extra[i].cat;
    return -1;
}

const char *command_level_name(int l) { return l == LEVEL_L1 ? "L1" : l == LEVEL_L2 ? "L2" : l == LEVEL_L3 ? "L3" : "L?"; }
const char *command_level_meaning(int l) {
    switch (l) {
        case LEVEL_L1: return "real - changes real kernel state";
        case LEVEL_L2: return "real - built on existing subsystems";
        case LEVEL_L3: return "SIMULATED - never touches real hardware";
        default:       return "unknown";
    }
}

/* ---------- planned phase (from the roadmap) ---------- */
typedef struct { const char *code; int phase; const char *what; } subsys_info_t;
static const subsys_info_t subsys_info[] = {
    { "DEV", 3, "devices" },   { "SYS", 3, "system/timer" }, { "CPU", 3, "CPU/interrupts" },
    { "SH", 4, "shell" },      { "MEM", 6, "memory manager" }, { "PROC", 7, "scheduler" },
    { "FS", 8, "MiniFS" },     { "BLK", 8, "RAM-disk" },     { "SEC", 10, "users/permissions" },
    { "LIB", 11, "text library" }, { "NET", 11, "network simulation" }, { "LOG", 12, "logger" },
};
#define SUBSYS_N (sizeof(subsys_info) / sizeof(subsys_info[0]))

static const subsys_info_t *latest_subsys(const char *subsys) {
    const subsys_info_t *best = NULL;
    const char *p = subsys;
    while (p && *p) {
        char tok[8]; size_t n = 0;
        while (*p && *p != '+' && n < sizeof(tok) - 1) tok[n++] = *p++;
        tok[n] = '\0';
        if (*p == '+') p++;
        for (size_t i = 0; i < SUBSYS_N; i++)
            if (str_eq(tok, subsys_info[i].code) && (!best || subsys_info[i].phase > best->phase)) best = &subsys_info[i];
    }
    return best;
}
int command_planned_phase(const command_t *c) {
    const subsys_info_t *s = c ? latest_subsys(c->subsys) : NULL;
    return s ? s->phase : 11;
}
const char *command_planned_phase_what(const command_t *c) {
    const subsys_info_t *s = c ? latest_subsys(c->subsys) : NULL;
    return s ? s->what : "kernel";
}

/* ---------- validation ---------- */
static const uint8_t expected_split[CAT_COUNT] = { 25, 35, 20, 30, 15, 20, 15, 25, 25, 30, 20, 20, 20 };

const char *command_validate_error(int code) {
    switch (code) {
        case CMDV_OK:             return "ok";
        case CMDV_COUNT:          return "wrong number of commands";
        case CMDV_EMPTY_NAME:     return "empty command name";
        case CMDV_BAD_NAME:       return "command name must be lowercase letters/digits";
        case CMDV_DUPLICATE:      return "duplicate command name";
        case CMDV_BAD_LEVEL:      return "level must be L1, L2 or L3";
        case CMDV_BAD_CATEGORY:   return "invalid category";
        case CMDV_BAD_ID:         return "id does not match position";
        case CMDV_NO_DESC:        return "missing description";
        case CMDV_CATEGORY_SPLIT: return "category sizes differ from the plan";
        case CMDV_NO_SUBSYS:      return "missing subsystem";
        default:                  return "unknown validation error";
    }
}

int command_validate(size_t *bad) {
    size_t dummy; if (!bad) bad = &dummy;
    *bad = 0;
    if (command_table_len != COMMAND_EXPECTED) return CMDV_COUNT;

    for (size_t i = 0; i < command_table_len; i++) {
        const command_t *c = &command_table[i];
        *bad = i;
        if (!c->name || !*c->name) return CMDV_EMPTY_NAME;
        for (const char *p = c->name; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) return CMDV_BAD_NAME;
        if (!c->desc || str_len(c->desc) == 0) return CMDV_NO_DESC;
        if (!c->subsys || str_len(c->subsys) == 0) return CMDV_NO_SUBSYS;
        if (c->level < LEVEL_L1 || c->level > LEVEL_L3) return CMDV_BAD_LEVEL;
        if (c->category >= CAT_COUNT) return CMDV_BAD_CATEGORY;
        if (c->id != i + 1) return CMDV_BAD_ID;
        for (size_t j = 0; j < i; j++)
            if (str_eq(command_table[j].name, c->name)) return CMDV_DUPLICATE;
    }
    for (int cat = 0; cat < CAT_COUNT; cat++)
        if (command_count_category((cmd_category_t)cat, NULL) != expected_split[cat]) { *bad = 0; return CMDV_CATEGORY_SPLIT; }
    return CMDV_OK;
}
