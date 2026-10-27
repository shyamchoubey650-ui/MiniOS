/* Networking commands (Phase 11): netinfo ifconfig ip route arp netstat host nslookup dig ping traceroute curl wget netsim.
 *
 * ALL SIMULATED. MiniOS has no network-card driver and no TCP/IP stack. These commands show a made-up virtual network
 * (addresses are QEMU's default user-mode network) so the command set can be demonstrated, and every output says so.
 * The one real part: curl/wget read files from /var/www in MiniFS, which acts as a "virtual local web server". */
#include "include/builtins.h"
#include "include/loopback.h"
#include "include/keyboard.h"
#include "include/cmdutil.h"
#include "include/screen.h"
#include "include/syscall.h"
#include "include/numparse.h"
#include "include/string.h"

#define SIM "[SIMULATED] "
#define WEB_ROOT "/var/www"

static const struct { const char *name, *ip; } hosts[] = {
    { "localhost", "127.0.0.1" }, { "minios.local", "10.0.2.15" }, { "gateway.local", "10.0.2.2" },
    { "dns.local", "10.0.2.3" }, { "example.test", "192.0.2.10" },
};
#define N_HOSTS (sizeof hosts / sizeof hosts[0])

static uint32_t sim_latency = 2, sim_loss;           /* netsim settings: milliseconds and percent */

static bool starts(const char *s, const char *p) { while (*p) { if (*s++ != *p++) return false; } return true; }
static void copy(char *dst, size_t cap, const char *src) { size_t i = 0; while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; } dst[i] = '\0'; }

static const char *lookup(const char *name) { for (size_t i = 0; i < N_HOSTS; i++) if (!strcmp(hosts[i].name, name)) return hosts[i].ip; return NULL; }
static const char *reverse(const char *ip) { for (size_t i = 0; i < N_HOSTS; i++) if (!strcmp(hosts[i].ip, ip)) return hosts[i].name; return NULL; }

static bool parse_ip(const char *s, uint32_t o[4]) {
    for (int i = 0; i < 4; i++) {
        uint32_t v = 0; size_t d = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s++ - '0'); if (++d > 3) return false; }
        if (d == 0 || v > 255) return false;
        o[i] = v;
        if (i < 3) { if (*s++ != '.') return false; }
    }
    return *s == '\0';
}
static bool resolve(const char *s, char *ip, size_t cap) {          /* name from the table, or a dotted address */
    uint32_t o[4]; const char *v = lookup(s);
    if (v) { copy(ip, cap, v); return true; }
    if (parse_ip(s, o)) { copy(ip, cap, s); return true; }
    return false;
}
static bool reachable(const char *ip) { return starts(ip, "127.") || starts(ip, "10.0.2.") || !strcmp(ip, "192.0.2.10"); }

static int unknown_host(const char *cmd, const char *name) {
    cu_color(VGA_LIGHT_RED); screen_puts("Error: "); cu_color(VGA_LIGHT_GREY); screen_puts(cmd); screen_puts(": unknown host '"); screen_puts(name); screen_puts("' (the virtual host table: see 'host')\n");
    return 1;
}

static int cmd_netinfo(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("netinfo");
    screen_puts("Networking in MiniOS is SIMULATED.\n");
    screen_puts("  - There is no network-card driver and no TCP/IP stack; nothing leaves the machine.\n");
    screen_puts("  - ifconfig, ip, route, arp, netstat, host, nslookup, dig, ping and traceroute print a fixed virtual network.\n");
    screen_puts("  - curl and wget are REAL file reads from "); screen_puts(WEB_ROOT); screen_puts(" in MiniFS (a pretend local web server).\n");
    screen_puts("  - 'netsim' changes the pretend latency and packet loss that ping reports.\n");
    return 0;
}

static void if_block(const char *name, const char *flags, const char *ip, const char *mask, const char *mac) {
    screen_puts(name); screen_puts(": flags=<"); screen_puts(flags); screen_puts(">\n");
    screen_puts("        inet "); screen_puts(ip); screen_puts("  netmask "); screen_puts(mask); screen_putc('\n');
    if (mac) { screen_puts("        ether "); screen_puts(mac); screen_putc('\n'); }
}
static int cmd_ifconfig(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("ifconfig");
    screen_puts(SIM "virtual interfaces (no hardware)\n");
    if_block("lo0", "UP,LOOPBACK,RUNNING", "127.0.0.1", "255.0.0.0", NULL);
    if_block("eth0", "UP,BROADCAST,RUNNING", "10.0.2.15", "255.255.255.0", "52:54:00:12:34:56");
    return 0;
}
static int cmd_route(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("route");
    screen_puts(SIM "virtual routing table\nDestination      Gateway          Iface\ndefault          10.0.2.2         eth0\n10.0.2.0/24      0.0.0.0          eth0\n127.0.0.0/8      0.0.0.0          lo0\n");
    return 0;
}
static int cmd_arp(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("arp");
    screen_puts(SIM "virtual ARP cache\nAddress          HWaddress           Iface\n10.0.2.2         52:55:0a:00:02:02   eth0\n10.0.2.3         52:55:0a:00:02:03   eth0\n");
    return 0;
}
static int cmd_ip(int argc, const char **argv) {
    const char *sub = argc >= 2 ? argv[1] : "addr";
    if (argc > 2) return cu_usage("ip [addr|route|neigh]");
    if (!strcmp(sub, "addr") || !strcmp(sub, "a")) return cmd_ifconfig(1, argv);
    if (!strcmp(sub, "route") || !strcmp(sub, "r")) return cmd_route(1, argv);
    if (!strcmp(sub, "neigh") || !strcmp(sub, "n")) return cmd_arp(1, argv);
    return cu_usage("ip [addr|route|neigh]");
}
static int cmd_netstat(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("netstat");
    screen_puts(SIM "virtual socket table\nProto  Local address        Foreign address      State\n");
    screen_puts("(empty: MiniOS has no socket API, so no program can have an open connection)\n");
    return 0;
}

static int cmd_host(int argc, const char **argv) {
    if (argc > 2) return cu_usage("host [name-or-address]     (no argument lists the virtual host table)");
    if (argc == 1) { screen_puts(SIM "virtual host table\n"); for (size_t i = 0; i < N_HOSTS; i++) { cu_pad(hosts[i].name, 16); screen_puts(hosts[i].ip); screen_putc('\n'); } return 0; }
    const char *ip = lookup(argv[1]);
    if (ip) { screen_puts(SIM); screen_puts(argv[1]); screen_puts(" has address "); screen_puts(ip); screen_putc('\n'); return 0; }
    const char *name = reverse(argv[1]);
    if (name) { screen_puts(SIM); screen_puts(argv[1]); screen_puts(" is named "); screen_puts(name); screen_putc('\n'); return 0; }
    return unknown_host("host", argv[1]);
}
static int cmd_nslookup(int argc, const char **argv) {
    if (argc != 2) return cu_usage("nslookup <name>");
    const char *ip = lookup(argv[1]);
    if (!ip) return unknown_host("nslookup", argv[1]);
    screen_puts(SIM "Server: 10.0.2.3 (virtual)\nName:    "); screen_puts(argv[1]); screen_puts("\nAddress: "); screen_puts(ip); screen_putc('\n');
    return 0;
}
static int cmd_dig(int argc, const char **argv) {
    if (argc != 2) return cu_usage("dig <name>");
    const char *ip = lookup(argv[1]);
    screen_puts(SIM "; <<>> virtual dig <<>> "); screen_puts(argv[1]); screen_putc('\n');
    screen_puts(";; status: "); screen_puts(ip ? "NOERROR" : "NXDOMAIN"); screen_puts(", server 10.0.2.3 (virtual), query time: "); screen_put_dec(sim_latency); screen_puts(" msec\n");
    if (!ip) return 1;
    screen_puts(";; ANSWER SECTION:\n"); screen_puts(argv[1]); screen_puts(".   300  IN  A  "); screen_puts(ip); screen_putc('\n');
    return 0;
}

static int cmd_ping(int argc, const char **argv) {
    uint32_t count = 4; char ip[20];
    if (argc < 2 || argc > 3 || (argc == 3 && (!cu_parse_count(argv[2], &count, 20) || count == 0))) return cu_usage("ping <host> [count 1-20]");
    if (!resolve(argv[1], ip, sizeof ip)) return unknown_host("ping", argv[1]);
    screen_puts(SIM "PING "); screen_puts(argv[1]); screen_puts(" ("); screen_puts(ip); screen_puts(") - generated replies, no packets are sent\n");
    if (!reachable(ip)) { screen_puts("Destination host unreachable (outside the virtual network)\n"); return 1; }
    bool loopback = starts(ip, "127.");
    uint32_t got = 0, total_ms = 0;
    for (uint32_t i = 0; i < count; i++) {
        bool lost = ((i + 1) * sim_loss) / 100 > (i * sim_loss) / 100;           /* losses are spread evenly */
        if (lost) { screen_puts("Request timeout for icmp_seq "); screen_put_dec(i + 1); screen_putc('\n'); continue; }
        uint32_t ms = loopback ? 0 : sim_latency + (i * 7) % 3;                  /* a little made-up jitter */
        got++; total_ms += ms;
        screen_puts("64 bytes from "); screen_puts(ip); screen_puts(": icmp_seq="); screen_put_dec(i + 1); screen_puts(" ttl=64 time="); screen_put_dec(ms); screen_puts(" ms\n");
    }
    screen_puts("--- "); screen_puts(argv[1]); screen_puts(" statistics ---\n");
    screen_put_dec(count); screen_puts(" sent, "); screen_put_dec(got); screen_puts(" received, "); screen_put_dec(((count - got) * 100) / count); screen_puts("% loss");
    if (got) { screen_puts(", average "); screen_put_dec(total_ms / got); screen_puts(" ms"); }
    screen_putc('\n');
    return got ? 0 : 1;
}

static int cmd_traceroute(int argc, const char **argv) {
    char ip[20];
    if (argc != 2) return cu_usage("traceroute <host>");
    if (!resolve(argv[1], ip, sizeof ip)) return unknown_host("traceroute", argv[1]);
    screen_puts(SIM "traceroute to "); screen_puts(argv[1]); screen_puts(" ("); screen_puts(ip); screen_puts("), generated hops\n");
    if (!reachable(ip)) { screen_puts(" 1  10.0.2.2  "); screen_put_dec(sim_latency); screen_puts(" ms\n 2  * * *   (outside the virtual network)\n"); return 1; }
    if (starts(ip, "127.")) { screen_puts(" 1  127.0.0.1  0 ms\n"); return 0; }
    if (strcmp(ip, "10.0.2.2")) { screen_puts(" 1  10.0.2.2  "); screen_put_dec(sim_latency); screen_puts(" ms\n 2  "); }
    else screen_puts(" 1  ");
    screen_puts(ip); screen_putc(' '); screen_put_dec(sim_latency + 1); screen_puts(" ms\n");
    return 0;
}

/* ---------- curl / wget: REAL reads from /var/www ---------- */
/* Accepts http://localhost/path, http://minios.local/path or a bare path; returns the MiniFS path. */
static bool url_to_path(const char *url, char *out, size_t cap) {
    const char *p = url;
    if (starts(p, "http://")) p += 7; else if (starts(p, "https://")) return false;
    if (p != url) {
        const char *slash = strchr(p, '/'); size_t hl = slash ? (size_t)(slash - p) : strlen(p);
        char host[32]; if (hl >= sizeof host) return false;
        memcpy(host, p, hl); host[hl] = '\0';
        if (strcmp(host, "localhost") && strcmp(host, "minios.local") && strcmp(host, "127.0.0.1")) return false;
        p = slash ? slash : "/";
    }
    if (*p == '/') p++;
    size_t n = strlen(WEB_ROOT);
    if (n + 1 + strlen(p) + 12 >= cap) return false;
    copy(out, cap, WEB_ROOT); out[n] = '/'; copy(out + n + 1, cap - n - 1, *p ? p : "index.html");
    if (strstr(out, "..")) return false;                                     /* no climbing out of the web root */
    return true;
}
static int fetch(const char *cmd, const char *url, char **body, size_t *len) {
    char path[FS_PATH_MAX];
    if (!url_to_path(url, path, sizeof path)) return cu_error(cmd, "only http://localhost/... or http://minios.local/... are reachable (the virtual server reads " WEB_ROOT ").");
    fs_stat_t st;
    int rc = sys_stat(path, &st);
    if (rc != 0 || st.type != FS_FILE) { screen_puts(SIM "HTTP 404 Not Found: "); screen_puts(path); screen_puts(" (create it with: mkdir -p " WEB_ROOT "; echo hi > " WEB_ROOT "/index.html)\n"); return 1; }
    return cu_load_file(cmd, path, body, len) ? 0 : 1;
}
static int cmd_curl(int argc, const char **argv) {
    char *b; size_t n;
    if (argc != 2) return cu_usage("curl <url>     e.g. curl http://localhost/index.html");
    if (fetch("curl", argv[1], &b, &n)) return 1;
    cu_print_text(b, n); if (n && b[n - 1] != '\n') screen_putc('\n');
    cu_free_file(b);
    return 0;
}
static int cmd_wget(int argc, const char **argv) {
    char *b; size_t n; const char *out = NULL, *url = NULL;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-O") && i + 1 < argc) out = argv[++i]; else if (!url) url = argv[i]; else return cu_usage("wget [-O file] <url>"); }
    if (!url) return cu_usage("wget [-O file] <url>     (saves into the current directory)");
    if (!out) { const char *s = url + strlen(url); while (s > url && s[-1] != '/') s--; out = *s ? s : "index.html"; }
    if (fetch("wget", url, &b, &n)) return 1;
    int fd = sys_open(out, SYS_O_WRITE | SYS_O_CREATE | SYS_O_TRUNC | SYS_O_EXCL);
    if (fd < 0) { cu_free_file(b); return cu_fs_error("wget", out, fd, CU_FILE); }
    int w = n ? sys_write(fd, b, n) : 0; sys_close(fd); cu_free_file(b);
    if (w < 0) return cu_fs_error("wget", out, w, CU_FILE);
    screen_puts("Saved "); screen_put_dec((uint32_t)n); screen_puts(" bytes to "); screen_puts(out); screen_putc('\n');
    return 0;
}

static int cmd_netsim(int argc, const char **argv) {
    uint32_t v;
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "show"))) { screen_puts(SIM "latency "); screen_put_dec(sim_latency); screen_puts(" ms, loss "); screen_put_dec(sim_loss); screen_puts("% (used only by ping/traceroute/dig output)\n"); return 0; }
    if (argc == 3 && !strcmp(argv[1], "latency")) { if (!cu_parse_count(argv[2], &v, 2000)) return cu_error("netsim", "latency is 0 to 2000 ms."); sim_latency = v; screen_puts("Pretend latency set.\n"); return 0; }
    if (argc == 3 && !strcmp(argv[1], "loss")) { if (!cu_parse_count(argv[2], &v, 100)) return cu_error("netsim", "loss is 0 to 100 percent."); sim_loss = v; screen_puts("Pretend packet loss set.\n"); return 0; }
    if (argc == 2 && !strcmp(argv[1], "reset")) { sim_latency = 2; sim_loss = 0; screen_puts("Simulation settings reset.\n"); return 0; }
    return cu_usage("netsim [show | latency <ms> | loss <percent> | reset]");
}

/* ---------- services, whois, telnet, ssh, ftp (all SIMULATED) ---------- */
static const struct { uint32_t port; const char *name; } services[] = { { 21, "ftp" }, { 22, "ssh" }, { 23, "telnet" }, { 53, "domain" }, { 80, "http" } };
static bool service_open(uint32_t port) { for (size_t i = 0; i < sizeof services / sizeof services[0]; i++) if (services[i].port == port) return true; return false; }

static const struct { const char *name; const char *text; } whois_db[] = {
    { "localhost",     "Host: localhost\nOwner: this machine (loopback)\nAddress: 127.0.0.1" },
    { "minios.local",  "Host: minios.local\nOwner: this MiniOS machine\nAddress: 10.0.2.15\nNetwork: 10.0.2.0/24 (virtual)" },
    { "gateway.local", "Host: gateway.local\nOwner: the virtual router\nAddress: 10.0.2.2\nRole: default gateway" },
    { "dns.local",     "Host: dns.local\nOwner: the virtual resolver\nAddress: 10.0.2.3\nRole: DNS server" },
    { "example.test",  "Domain: example.test\nRegistrant: Example Registrant (made-up record)\nAddress: 192.0.2.10 (documentation range, RFC 5737)\nStatus: reserved for examples" },
};
static int cmd_whois(int argc, const char **argv) {
    if (argc != 2) return cu_usage("whois <name>");
    for (size_t i = 0; i < sizeof whois_db / sizeof whois_db[0]; i++)
        if (!strcmp(argv[1], whois_db[i].name)) { screen_puts(SIM "static record\n"); screen_puts(whois_db[i].text); screen_putc('\n'); return 0; }
    screen_puts(SIM "No match for '"); screen_puts(argv[1]); screen_puts("' (the made-up database has "); screen_put_dec((uint32_t)(sizeof whois_db / sizeof whois_db[0])); screen_puts(" records: see 'host')\n");
    return 1;
}

/* Common start of telnet / ssh / ftp: resolves the host and decides whether anything answers on the port. */
static int connect_check(const char *cmd, const char *host, uint32_t port, char *ip, size_t ipcap) {
    if (!resolve(host, ip, ipcap)) return unknown_host(cmd, host);
    screen_puts(SIM "Trying "); screen_puts(ip); screen_puts("...\n");
    if (!reachable(ip)) { screen_puts(cmd); screen_puts(": connect to "); screen_puts(host); screen_puts(": Connection timed out (outside the virtual network)\n"); return 1; }
    if (!service_open(port)) { screen_puts(cmd); screen_puts(": connect to "); screen_puts(host); screen_puts(" port "); screen_put_dec(port); screen_puts(": Connection refused\n"); return 1; }
    return 0;
}
static int cmd_telnet(int argc, const char **argv) {
    uint32_t port = 23; char ip[20];
    if (argc < 2 || argc > 3 || (argc == 3 && (!cu_parse_count(argv[2], &port, 65535) || port == 0))) return cu_usage("telnet <host> [port]      (simulated: nothing is really connected)");
    int rc = connect_check("telnet", argv[1], port, ip, sizeof ip);
    if (rc) return rc;
    screen_puts("Connected to "); screen_puts(argv[1]); screen_puts(".\nEscape character is '^]'.\n\n");
    screen_puts(argv[1]); screen_puts(" virtual terminal (SIMULATED session: no server is listening)\nlogin: \nConnection closed by foreign host.\n");
    return 0;
}
static int cmd_ssh(int argc, const char **argv) {
    char user[20], host[40], ip[20]; const char *target;
    if (argc != 2) return cu_usage("ssh [user@]host      (simulated: shows what a first connection looks like, then refuses)");
    target = argv[1];
    const char *at = target; while (*at && *at != '@') at++;
    if (*at == '@') { size_t n = (size_t)(at - target); if (n == 0 || n >= sizeof user) return cu_error("ssh", "bad user name"); memcpy(user, target, n); user[n] = '\0'; copy(host, sizeof host, at + 1); }
    else { cred_info_t c; if (sys_getcred(&c) == 0) copy(user, sizeof user, c.name); else copy(user, sizeof user, "user"); copy(host, sizeof host, target); }
    if (!host[0]) return cu_error("ssh", "bad host name");
    int rc = connect_check("ssh", host, 22, ip, sizeof ip);
    if (rc) return rc;
    uint32_t h = 2166136261u; for (size_t i = 0; host[i]; i++) { h ^= (uint8_t)host[i]; h *= 16777619u; }
    screen_puts("The authenticity of host '"); screen_puts(host); screen_puts(" ("); screen_puts(ip); screen_puts(")' can't be established.\nMade-up key fingerprint: ");
    for (int i = 0; i < 8; i++) { h = h * 1664525u + 1013904223u; static const char hx[] = "0123456789abcdef"; screen_putc(hx[(h >> 28) & 15]); screen_putc(hx[(h >> 24) & 15]); if (i < 7) screen_putc(':'); }
    screen_puts("\n"); screen_puts(user); screen_putc('@'); screen_puts(host); screen_puts(": Permission denied (SIMULATED: there is no ssh server and no session)\n");
    return 255;
}
static const struct { const char *name; uint32_t size; } ftp_files[] = { { "readme.txt", 64 }, { "data.csv", 128 } };
static int cmd_ftp(int argc, const char **argv) {
    char ip[20];
    if (argc < 2 || argc > 3) return cu_usage("ftp <host> [file]      (simulated: lists a made-up directory, transfers nothing)");
    int rc = connect_check("ftp", argv[1], 21, ip, sizeof ip);
    if (rc) return rc;
    screen_puts("Connected to "); screen_puts(argv[1]); screen_puts(".\n220 virtual FTP service ready (SIMULATED)\nName: anonymous\n230 Login successful.\n");
    if (argc == 2) {
        screen_puts("ftp> ls\n");
        for (size_t i = 0; i < sizeof ftp_files / sizeof ftp_files[0]; i++) { screen_puts("-rw-r--r--  1 ftp  ftp  "); cu_num_pad(ftp_files[i].size, 5); screen_putc(' '); screen_puts(ftp_files[i].name); screen_putc('\n'); }
        screen_puts("226 Directory send OK.\n");
    } else {
        bool found = false; for (size_t i = 0; i < sizeof ftp_files / sizeof ftp_files[0]; i++) if (!strcmp(argv[2], ftp_files[i].name)) found = true;
        screen_puts("ftp> get "); screen_puts(argv[2]); screen_putc('\n');
        if (!found) { screen_puts("550 "); screen_puts(argv[2]); screen_puts(": No such file or directory.\n221 Goodbye.\n"); return 1; }
        screen_puts("226 Transfer complete (SIMULATED: no bytes were written; use 'wget' for the virtual web pages).\n");
    }
    screen_puts("221 Goodbye.\n");
    return 0;
}

/* ---------- loopback and nc (the queue is REAL) ---------- */
static bool ctrl_c_pending(void) { bool hit = false; while (keyboard_has_key()) if (keyboard_getkey() == 3) hit = true; return hit; }
static bool is_root_user(void) { cred_info_t c; return sys_getcred(&c) == 0 && c.euid == 0; }
static bool may_receive(uint32_t port) { return port >= 1024 || is_root_user(); }
static int lo_fail(const char *cmd, int rc) { return cu_error(cmd, lo_strerror(rc)); }

/* Waits up to `secs` for a message on `port`. Returns the length, LO_ERR_EMPTY on timeout, or -100 if interrupted. */
static int lo_wait(uint32_t port, char *buf, size_t cap, uint32_t secs) {
    for (uint32_t i = 0; i <= secs * 10; i++) {
        int n = lo_recv(port, buf, cap);
        if (n >= 0 || n != LO_ERR_EMPTY) return n;
        if (i == secs * 10 || ctrl_c_pending()) return i == secs * 10 ? LO_ERR_EMPTY : -100;
        cu_sleep_ms(100);
    }
    return LO_ERR_EMPTY;
}
static int joined_text(int argc, const char **argv, int first, char *buf, size_t cap) {
    size_t w = 0;
    for (int i = first; i < argc; i++) { if (i > first) { if (w + 1 >= cap) return -1; buf[w++] = ' '; } for (const char *p = argv[i]; *p; p++) { if (w + 1 >= cap) return -1; buf[w++] = *p; } }
    buf[w] = '\0';
    return (int)w;
}

static int lb_status(void) {
    uint32_t port, pend, sent; size_t i = 0;
    screen_puts("loopback queue (REAL, in memory): "); screen_put_dec(lo_total_sent()); screen_puts(" messages sent since boot\n");
    screen_puts("port   waiting  sent\n");
    for (; lo_port_at(i, &port, &pend, &sent); i++) { cu_num_pad(port, 5); cu_num_pad(pend, 9); cu_num_pad(sent, 6); screen_putc('\n'); }
    if (i == 0) screen_puts("(no port has a queue yet)\n");
    screen_puts("Try: loopback demo   |   loopback send <port> <text>   |   loopback recv <port>\n");
    return 0;
}
static int lb_demo(void) {
    int pid = sys_process_create("lbecho", 10, 3);
    if (pid < 0) return cu_error("loopback", sys_strerror(pid));
    screen_puts("started the echo task (pid "); screen_put_dec((uint32_t)pid); screen_puts("); it answers port 7777 on port 7778\n");
    static const char *const words[] = { "hello", "MiniOS", "loopback" };
    uint32_t got = 0;
    for (size_t i = 0; i < 3; i++) {
        int rc = lo_send(LO_ECHO_IN, words[i]);
        if (rc != LO_OK) return lo_fail("loopback", rc);
        screen_puts("sent    : "); screen_puts(words[i]); screen_putc('\n');
    }
    for (size_t i = 0; i < 3; i++) {
        char r[LO_MSG_MAX + 1]; int n = lo_wait(LO_ECHO_OUT, r, sizeof r, 3);
        if (n < 0) break;
        screen_puts("received: "); screen_puts(r); screen_putc('\n'); got++;
    }
    lo_send(LO_ECHO_IN, "quit");
    screen_put_dec(got); screen_puts(" of 3 replies arrived");
    if (got < 3) screen_puts(" (the echo task did not answer in time: is it still starting? try again, or check 'ps')");
    screen_putc('\n');
    return got == 3 ? 0 : 1;
}
static int cmd_loopback(int argc, const char **argv) {
    uint32_t port; char text[LO_MSG_MAX + 1], r[LO_MSG_MAX + 1];
    if (argc == 1) return lb_status();
    if (!strcmp(argv[1], "demo") && argc == 2) return lb_demo();
    if (!strcmp(argv[1], "send") && argc >= 4) {
        if (!cu_parse_count(argv[2], &port, 65535) || port == 0) return cu_error("loopback", lo_strerror(LO_ERR_PORT));
        if (joined_text(argc, argv, 3, text, sizeof text) < 0) return lo_fail("loopback", LO_ERR_LONG);
        int rc = lo_send(port, text); if (rc != LO_OK) return lo_fail("loopback", rc);
        screen_puts("queued for port "); screen_put_dec(port); screen_puts(": "); screen_puts(text); screen_putc('\n');
        return 0;
    }
    if (!strcmp(argv[1], "recv") && (argc == 3 || (argc == 5 && !strcmp(argv[3], "-w")))) {
        uint32_t wait = 0;
        if (!cu_parse_count(argv[2], &port, 65535) || port == 0) return cu_error("loopback", lo_strerror(LO_ERR_PORT));
        if (argc == 5 && (!cu_parse_count(argv[4], &wait, 60))) return cu_error("loopback", "-w needs 0-60 seconds");
        if (!may_receive(port)) return cu_error("loopback", "receiving on a port below 1024 needs root (try sudo)");
        int n = lo_wait(port, r, sizeof r, wait);
        if (n == -100) { screen_puts("interrupted\n"); return 130; }
        if (n < 0) { screen_puts("(no message waiting on port "); screen_put_dec(port); screen_puts(")\n"); return 1; }
        screen_puts(r); screen_putc('\n');
        return 0;
    }
    if (!strcmp(argv[1], "clear") && argc <= 3) {
        if (!is_root_user()) return cu_error("loopback", "permission denied: only root can clear queues (try sudo)");
        if (argc == 3) { if (!cu_parse_count(argv[2], &port, 65535) || port == 0) return cu_error("loopback", lo_strerror(LO_ERR_PORT)); lo_clear(port); }
        else lo_clear(0);
        screen_puts("cleared\n");
        return 0;
    }
    return cu_usage("loopback   |   loopback demo   |   loopback send <port> <text...>   |   loopback recv <port> [-w seconds]   |   loopback clear [port]");
}
static bool is_local_host(const char *ip) { return starts(ip, "127.") || !strcmp(ip, "10.0.2.15"); }
static int cmd_nc(int argc, const char **argv) {
    uint32_t port, wait = 5; char text[LO_MSG_MAX + 1], r[LO_MSG_MAX + 1], ip[20];
    if (argc >= 3 && !strcmp(argv[1], "-l")) {
        if (argc > 5 || (argc == 5 && strcmp(argv[3], "-w")) || argc == 4) return cu_usage("nc -l <port> [-w seconds]");
        if (!cu_parse_count(argv[2], &port, 65535) || port == 0) return cu_error("nc", lo_strerror(LO_ERR_PORT));
        if (argc == 5 && !cu_parse_count(argv[4], &wait, 60)) return cu_error("nc", "-w needs 0-60 seconds");
        if (!may_receive(port)) return cu_error("nc", "listening on a port below 1024 needs root (try sudo)");
        screen_puts("listening on loopback port "); screen_put_dec(port); screen_puts(" for "); screen_put_dec(wait); screen_puts(" s (Ctrl+C stops)...\n");
        int n = lo_wait(port, r, sizeof r, wait);
        if (n == -100) { screen_puts("interrupted\n"); return 130; }
        if (n < 0) { screen_puts("nothing arrived\n"); return 1; }
        screen_puts(r); screen_putc('\n');
        return 0;
    }
    if (argc < 4) return cu_usage("nc <host> <port> <text...>   |   nc -l <port> [-w seconds]      (localhost uses the REAL loopback queue; other hosts are simulated)");
    if (!cu_parse_count(argv[2], &port, 65535) || port == 0) return cu_error("nc", lo_strerror(LO_ERR_PORT));
    if (!resolve(argv[1], ip, sizeof ip)) return unknown_host("nc", argv[1]);
    if (joined_text(argc, argv, 3, text, sizeof text) < 0) return lo_fail("nc", LO_ERR_LONG);
    if (is_local_host(ip)) {
        int rc = lo_send(port, text); if (rc != LO_OK) return lo_fail("nc", rc);
        screen_puts("sent "); screen_put_dec((uint32_t)strlen(text)); screen_puts(" bytes to "); screen_puts(argv[1]); screen_putc(':'); screen_put_dec(port); screen_puts(" through the loopback queue (receive with: nc -l "); screen_put_dec(port); screen_puts(")\n");
        return 0;
    }
    screen_puts(SIM);
    if (!reachable(ip)) { screen_puts("nc: connect to "); screen_puts(argv[1]); screen_puts(": Connection timed out (outside the virtual network)\n"); return 1; }
    if (!service_open(port)) { screen_puts("nc: connect to "); screen_puts(argv[1]); screen_puts(" port "); screen_put_dec(port); screen_puts(": Connection refused\n"); return 1; }
    screen_puts("connection to "); screen_puts(argv[1]); screen_puts(" port "); screen_put_dec(port); screen_puts(" succeeded; "); screen_put_dec((uint32_t)strlen(text)); screen_puts(" bytes were NOT sent anywhere\n");
    return 0;
}


static const struct { const char *name; cmd_handler_t fn; } bindings[] = {
    { "netinfo", cmd_netinfo }, { "ifconfig", cmd_ifconfig }, { "ip", cmd_ip }, { "route", cmd_route }, { "arp", cmd_arp }, { "netstat", cmd_netstat },
    { "host", cmd_host }, { "nslookup", cmd_nslookup }, { "dig", cmd_dig }, { "ping", cmd_ping }, { "traceroute", cmd_traceroute },
    { "curl", cmd_curl }, { "wget", cmd_wget }, { "netsim", cmd_netsim },
    { "whois", cmd_whois }, { "telnet", cmd_telnet }, { "ssh", cmd_ssh }, { "ftp", cmd_ftp }, { "nc", cmd_nc }, { "loopback", cmd_loopback },
};

int network_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
