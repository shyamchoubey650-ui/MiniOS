#ifndef BUILTINS_H
#define BUILTINS_H
#include "include/command.h"
/* Kernel-side command handlers (commands/shell_commands.c). */
int builtins_register(void);   /* binds every implemented handler; returns number of FAILED bindings */
int memory_commands_register(void);   /* commands/memory_commands.c (Phase 6) */
int process_commands_register(void);  /* commands/process_commands.c (Phase 7) */
int directory_commands_register(void); /* Phase 8 */
int file_commands_register(void);
int text_commands_register(void);
int fsinfo_commands_register(void);
int syscall_commands_register(void);   /* Phase 9 */
int shellenv_commands_register(void);   /* Phase 11 */
int textops_commands_register(void);
int sysinfo_commands_register(void);
int disk_commands_register(void);
int misc_commands_register(void);
int network_commands_register(void);
int diag_commands_register(void);
int fileextra_commands_register(void);
int log_commands_register(void);       /* Phase 12: logs dmesg loglevel logclear logsave logtail logfind logstats trace strace schedtrace */
int jobs_commands_register(void);      /* Phase 12: bg fg memlimit */
int blkextra_commands_register(void);  /* Phase 12: mount umount defrag smart */
typedef int (*shell_exec_fn)(const char *line);
void shellenv_set_executor(shell_exec_fn fn);   /* the shell hands over its line executor (source, demo, repeat use it) */
int user_commands_register(void);      /* Phase 10: users, sessions, ownership */
int command_stub(const command_t *c, int argc, const char **argv);   /* what an unimplemented command runs */
#endif
