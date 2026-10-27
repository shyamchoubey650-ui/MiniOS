/* The real lists behind Tab completion: the command table, the shell variables and MiniFS directories (read through the system calls,
 * so a user is only offered names he is allowed to list). */
#include "include/complete.h"
#include "include/command.h"
#include "include/shellvars.h"
#include "include/syscall.h"
#include "include/memory.h"

static void list_commands(comp_emit_fn emit, void *ctx) {
    for (size_t i = 0; i < command_count(); i++) { const command_t *c = command_at(i); if (c) emit(c->name, false, ctx); }
}
static void list_variables(comp_emit_fn emit, void *ctx) {
    const char *name, *value; bool exported;
    for (size_t i = 0; var_at(i, &name, &value, &exported); i++) emit(name, false, ctx);
}
static void list_entries(const char *dir, comp_emit_fn emit, void *ctx) {
    const char *path = dir[0] ? dir : ".";
    int total = sys_readdir(path, NULL, 0);
    if (total <= 0) return;                                        /* missing, empty, or not readable by this user */
    fs_dirent_t *list = kmalloc(sizeof(fs_dirent_t) * (size_t)total);
    if (!list) return;
    int n = sys_readdir(path, list, (uint32_t)total);
    for (int i = 0; i < n; i++) emit(list[i].name, list[i].type == FS_DIR, ctx);
    kfree(list);
}

const comp_env_t *complete_env_system(void) {
    static const comp_env_t env = { list_commands, list_variables, list_entries };
    return &env;
}
