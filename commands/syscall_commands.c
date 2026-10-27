/* Commands built on the system-call interface (Phase 9): sysinfo uname hostname errno fdlist fdinfo.
 * They use sys_* calls only. */
#include "include/builtins.h"
#include "include/cmdutil.h"
#include "include/screen.h"
#include "include/syscall.h"
#include "include/version.h"
#include "include/numparse.h"
#include "include/string.h"

#define HOSTNAME_FILE "/etc/hostname"

static void put_flags(uint32_t f) {
    if (f & SYS_O_READ) screen_putc('r');
    if (f & SYS_O_WRITE) screen_putc((f & SYS_O_APPEND) ? 'a' : 'w');
}

/* Reads /etc/hostname (without the trailing newline) into out. Returns 0 or a SYS_E* code. */
static int read_hostname(char *out, size_t cap) {
    int fd = sys_open(HOSTNAME_FILE, SYS_O_READ);
    if (fd < 0) return fd;
    int n = sys_read(fd, out, cap - 1);
    sys_close(fd);
    if (n < 0) return n;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) n--;
    out[n] = '\0';
    return 0;
}

static int cmd_sysinfo(int argc, const char **argv) {
    (void)argc; (void)argv;
    sys_info_t i;
    int rc = sys_sysinfo(&i);
    if (rc != 0) return cu_error("sysinfo", sys_strerror(rc));
    char host[40]; if (read_hostname(host, sizeof host) != 0) memcpy(host, "unknown", 8);
    screen_puts("MiniOS system summary\n");
    screen_puts("  Version     : "); screen_puts(MINIOS_VERSION); screen_puts(" ("); screen_puts(MINIOS_PHASE); screen_puts("), i386, educational\n");
    screen_puts("  Hostname    : "); screen_puts(host); screen_putc('\n');
    screen_puts("  Uptime      : "); screen_put_dec(i.uptime_ticks / i.ticks_per_sec); screen_puts(" s ("); screen_put_dec(i.uptime_ticks); screen_puts(" ticks at "); screen_put_dec(i.ticks_per_sec); screen_puts(" Hz)\n");
    screen_puts("  Memory      : "); cu_put_size(i.ram_bytes); screen_puts(" RAM, "); cu_put_size(i.ram_free_bytes); screen_puts(" free\n");
    screen_puts("  Kernel heap : "); cu_put_size(i.heap_total); screen_puts(", "); cu_put_size(i.heap_used); screen_puts(" used, "); cu_put_size(i.heap_free); screen_puts(" free\n");
    screen_puts("  Processes   : "); screen_put_dec(i.processes); screen_puts(" live, "); screen_put_dec(i.context_switches); screen_puts(" context switches\n");
    screen_puts("  File system : MiniFS (RAM), "); screen_put_dec(i.fs_nodes); screen_putc('/'); screen_put_dec(i.fs_max_nodes); screen_puts(" nodes, "); cu_put_size(i.fs_data_bytes); screen_puts(" of data\n");
    screen_puts("  System calls: "); screen_put_dec(i.syscalls_total); screen_puts(" made, "); screen_put_dec(i.syscalls_failed); screen_puts(" returned an error\n");
    return 0;
}

static int cmd_uname(int argc, const char **argv) {
    bool all = argc == 2 && !strcmp(argv[1], "-a");
    if (argc > 2 || (argc == 2 && !all)) return cu_usage("uname [-a]");
    if (!all) { screen_puts("MiniOS\n"); return 0; }
    char host[40]; if (read_hostname(host, sizeof host) != 0) memcpy(host, "unknown", 8);
    screen_puts("MiniOS "); screen_puts(host); screen_putc(' '); screen_puts(MINIOS_VERSION); screen_putc(' '); screen_puts(MINIOS_PHASE); screen_puts(" i386\n");
    return 0;
}

static bool valid_hostname(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 31) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return s[0] != '-';
}

static int cmd_hostname(int argc, const char **argv) {
    if (argc > 2) return cu_usage("hostname [new-name]");
    if (argc == 2) {
        if (!valid_hostname(argv[1])) return cu_error("hostname", "use 1-31 letters, digits or '-' (not starting with '-')");
        char line[40]; size_t n = strlen(argv[1]);
        memcpy(line, argv[1], n); line[n++] = '\n';
        int fd = sys_open(HOSTNAME_FILE, SYS_O_WRITE | SYS_O_CREATE | SYS_O_TRUNC);
        if (fd < 0) return cu_fs_error("hostname", HOSTNAME_FILE, fd, CU_FILE);
        int w = sys_write(fd, line, n);
        sys_close(fd);
        return w < 0 ? cu_fs_error("hostname", HOSTNAME_FILE, w, CU_FILE) : 0;
    }
    char host[40];
    int rc = read_hostname(host, sizeof host);
    if (rc != 0) return cu_fs_error("hostname", HOSTNAME_FILE, rc, CU_FILE);
    screen_puts(host); screen_putc('\n');
    return 0;
}

/* errno <number | NAME>: explains a system-call error code. */
static bool same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++) { char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a; if (x != *b) return false; }
    return *a == *b;
}
static int cmd_errno(int argc, const char **argv) {
    if (argc != 2) return cu_usage("errno <code | NAME>     e.g. errno -1, errno 15, errno EBADF");
    int code = 1;
    bool found = false;
    const char *s = argv[1];
    bool neg = *s == '-';
    uint32_t v;
    if (parse_u32(neg ? s + 1 : s, &v) && v <= 1000) { code = -(int)v; found = true; }
    else for (int c = 0; c >= SYS_ERR_MIN; c--) if (same_name(s, sys_errname(c)) && sys_errname(c)[0] != '?') { code = c; found = true; break; }
    if (!found || code < SYS_ERR_MIN || code > 0) {
        cu_error("errno", "no such error code");
        screen_puts("       valid codes are 0 to -"); screen_put_dec((uint32_t)(-SYS_ERR_MIN)); screen_puts(", or a name such as ENOENT\n");
        return 1;
    }
    screen_put_dec(code == 0 ? 0 : (uint32_t)(-code)); screen_puts(code == 0 ? " " : " (negative) "); screen_puts(sys_errname(code)); screen_puts(": "); screen_puts(sys_strerror(code)); screen_putc('\n');
    return 0;
}

static int cmd_fdlist(int argc, const char **argv) {
    (void)argc; (void)argv;
    int shown = 0;
    screen_puts("FD  MODE  OFFSET  SIZE    PATH\n");
    for (int fd = 0; fd < SYS_MAX_FDS; fd++) {
        sys_fdinfo_t f;
        if (sys_fdinfo(fd, &f) != 0) continue;
        cu_num_pad((uint32_t)fd, 2); screen_puts("  ");
        char m[4] = "   "; size_t k = 0;
        if (f.flags & SYS_O_READ) m[k++] = 'r';
        if (f.flags & SYS_O_WRITE) m[k++] = (f.flags & SYS_O_APPEND) ? 'a' : 'w';
        cu_pad(m, 6);
        if (fd <= 2) screen_puts("-       -       console");
        else { cu_num_pad(f.offset, 6); screen_puts("  "); cu_num_pad(f.size, 6); screen_puts("  "); screen_puts(f.path); if (f.stale) screen_puts("  (stale)"); }
        screen_putc('\n'); shown++;
    }
    screen_puts("(descriptors 0-2 are the console; files opened by commands are closed again when the command ends)\n");
    return shown ? 0 : 1;
}

static int cmd_fdinfo(int argc, const char **argv) {
    uint32_t fd;
    if (argc != 2) return cu_usage("fdinfo <fd>");
    if (!parse_u32(argv[1], &fd) || fd >= SYS_MAX_FDS) return cu_error("fdinfo", "descriptor must be 0 to 15");
    sys_fdinfo_t f;
    int rc = sys_fdinfo((int)fd, &f);
    if (rc != 0) return cu_error("fdinfo", sys_strerror(rc));
    screen_puts("Descriptor "); screen_put_dec(fd); screen_putc('\n');
    screen_puts("  Target : "); screen_puts(f.path); screen_putc('\n');
    screen_puts("  Access : "); put_flags(f.flags); screen_putc('\n');
    if (fd > 2) { screen_puts("  Offset : "); screen_put_dec(f.offset); screen_putc('\n'); screen_puts("  Size   : "); screen_put_dec(f.size); screen_puts(f.stale ? " (file was removed or replaced: stale)\n" : "\n"); }
    return 0;
}

static const struct { const char *name; cmd_handler_t fn; } bindings[] = {
    { "sysinfo", cmd_sysinfo }, { "uname", cmd_uname }, { "hostname", cmd_hostname },
    { "errno", cmd_errno }, { "fdlist", cmd_fdlist }, { "fdinfo", cmd_fdinfo },
};

int syscall_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
