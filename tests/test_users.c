/* Phase 10 tests: accounts, password hashes, login/logout/su/sudo, ownership, umask, the audit trail,
 * and every user command, all through the REAL system-call layer, user manager, MiniFS, parser, registry and
 * redirection. Only the screen, keyboard, timer and context switch are stubbed. The keyboard stub feeds
 * the passwords that the commands ask for, and an empty queue answers Ctrl+C so a prompt can never hang. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/shell.h"
#include "include/command.h"
#include "include/builtins.h"
#include "include/memory.h"
#include "include/heap.h"
#include "include/filesystem.h"
#include "include/redirect.h"
#include "include/capture.h"
#include "include/screen.h"
#include "include/timer.h"
#include "include/interrupts.h"
#include "include/syscall.h"
#include "include/process.h"
#include "include/users.h"
#include "tests/low_stack.h"

void interrupt_register(uint32_t v, interrupt_handler_t h) { (void)v; (void)h; }
static char out[65536]; static size_t out_len;
static void out_reset(void) { out_len = 0; out[0] = 0; }
void screen_init(void) {} void screen_clear(void) {} void screen_set_color(uint8_t f, uint8_t b) { (void)f; (void)b; } void screen_set_default_color(uint8_t f, uint8_t b) { (void)f; (void)b; }
void screen_backspace(void) {}
void screen_putc(char c) { if (capture_write(c)) return; if (out_len + 1 < sizeof out) { out[out_len++] = c; out[out_len] = 0; } }
void screen_puts(const char *s) { while (*s) screen_putc(*s++); }
void screen_put_dec(uint32_t v) { char b[16]; snprintf(b, sizeof b, "%u", v); screen_puts(b); }
void screen_put_hex(uint32_t v) { char b[16]; snprintf(b, sizeof b, "0x%08X", v); screen_puts(b); }
static uint32_t fake_ticks = 500;
uint32_t timer_ticks(void) { return fake_ticks; }
void timer_sleep_ms(uint32_t ms) { (void)ms; }
const char *exception_name(uint32_t v) { (void)v; return "exception"; }
int shell_last_exit(void) { return 0; }
int memory_commands_register(void) { return 0; }
int process_commands_register(void) { return 0; }

/* keyboard: text queued by feed() is what the next password prompts read */
static char kq[512]; static size_t kq_pos;
static void feed(const char *s) { strncpy(kq, s, sizeof kq - 1); kq[sizeof kq - 1] = 0; kq_pos = 0; }
static bool fed_all(void) { return kq[kq_pos] == 0; }
int keyboard_has_key(void) { return kq[kq_pos] != 0; }
int keyboard_getkey(void) { return kq[kq_pos] ? (unsigned char)kq[kq_pos++] : 3; }

static int run, failed;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; printf("  FAIL line %d: %s\n    output was: [%s]\n", __LINE__, #cond, out); } } while (0)

static bool owner_yes(void) { return true; }
static int sh(const char *line) {
    static char buf[SHELL_LINE_MAX * 2]; command_line_t cl;
    out_reset();
    int rc = parse_command_line(line, buf, sizeof buf, &cl);
    if (rc == PARSE_EMPTY) return 0;
    if (rc != PARSE_OK) { screen_puts("Error: "); screen_puts(parse_error_string(rc)); screen_putc('\n'); return 2; }
    const command_t *c = command_find(cl.argv[0]);
    if (!c) { screen_puts("Error: unknown command '"); screen_puts(cl.argv[0]); screen_puts("'\n"); return 127; }
    redirect_t rd; bool redirected = false;
    if (cl.redir != REDIR_NONE) {
        int rrc = redirect_begin(&rd, cl.redir_path, cl.redir, owner_yes);
        if (rrc != FS_OK) { screen_puts("Error: cannot redirect to '"); screen_puts(cl.redir_path); screen_puts("': "); screen_puts(fs_strerror(rrc)); screen_putc('\n'); return 1; }
        redirected = true;
    }
    int st = c->handler ? c->handler(cl.argc, cl.argv) : command_stub(c, cl.argc, cl.argv);
    if (redirected) st = redirect_end(&rd, st);
    return st;
}
static bool has(const char *s) { return strstr(out, s) != NULL; }

static multiboot_mmap_entry_t map_buf[1]; static multiboot_info_t mbi;
static void fresh(void) {
    map_buf[0] = (multiboot_mmap_entry_t){ 20, 0x100000, 0x4000000, 1 };
    memset(&mbi, 0, sizeof mbi);
    mbi.flags = MULTIBOOT_FLAG_MMAP; mbi.mmap_addr = (uint32_t)(uintptr_t)map_buf; mbi.mmap_length = sizeof map_buf;
    memory_init(&mbi, 0x100000, 0x110000);
    fs_set_clock(timer_ticks); fs_set_user(FS_UID_ROOT, FS_UID_ROOT); fs_set_umask(022);
    fs_init(); process_init(); syscall_init();
    users_init();
    kq[0] = 0; kq_pos = 0; out_reset();
}

/* ---- helpers ---- */
#define login_as(u, p) do { int rc_ = sys_login(u, p); if (rc_ != 0) { failed++; printf("  FAIL line %d: login_as(%s) = %d\n", __LINE__, u, rc_); } run++; } while (0)
static uint16_t mode_of(const char *path) { fs_stat_t st; return sys_stat(path, &st) == 0 ? st.mode : 0xFFFF; }
static int owner_of(const char *path) { fs_stat_t st; return sys_stat(path, &st) == 0 ? (int)st.uid : -1; }
static int group_of(const char *path) { fs_stat_t st; return sys_stat(path, &st) == 0 ? (int)st.gid : -1; }
static int write_file(const char *path, const char *text) {
    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREATE | SYS_O_TRUNC);
    if (fd < 0) return fd;
    int n = sys_write(fd, text, strlen(text)); sys_close(fd);
    return n < 0 ? n : 0;
}
static bool file_text_is(const char *path, const char *want) {
    char b[512]; int fd = sys_open(path, SYS_O_READ); if (fd < 0) return false;
    int n = sys_read(fd, b, sizeof b - 1); sys_close(fd); if (n < 0) return false; b[n] = 0;
    return strstr(b, want) != NULL;
}
static int event_count(void) { return sys_audit_get(0, NULL); }

static void test_defaults(void) {
    puts("built-in accounts and stored passwords");
    cred_info_t ci; user_info_t u;
    CHECK(sys_getuid() == SYS_ENOUSER && sys_getcred(&ci) == 0 && ci.depth == 0 && ci.euid == (int)UID_NOBODY && !strcmp(ci.name, "nobody"));
    uint16_t fu, fg; fs_get_user(&fu, &fg); CHECK(fu == UID_NOBODY && fg == UID_NOBODY);                /* logged out: MiniFS acts for nobody */
    static const char *names[] = { "root", "admin", "student", "guest" };
    for (uint32_t i = 0; i < 4; i++) CHECK(sys_user_get(i, &u) == 0 && !strcmp(u.name, names[i]) && u.uid == i && u.gid == i);
    CHECK(sys_user_get(4, &u) == SYS_ENOENT && sys_user_get(0, NULL) == SYS_EFAULT);
    CHECK(sys_user_lookup("admin", 0, &u) == 0 && (u.flags & USER_F_SUDO) && !strcmp(u.home, "/home/admin"));
    CHECK(sys_user_lookup("root", 0, &u) == 0 && !strcmp(u.home, "/root") && !(u.flags & USER_F_SUDO));
    CHECK(sys_user_lookup("student", 0, &u) == 0 && !(u.flags & (USER_F_SUDO | USER_F_LOCKED)));
    CHECK(sys_user_lookup(NULL, 3, &u) == 0 && !strcmp(u.name, "guest") && sys_user_lookup(NULL, 99, &u) == SYS_ENOUSER && sys_user_lookup("nobody", 0, &u) == SYS_ENOUSER);
    CHECK(sys_verify("root", "root123") == 0 && sys_verify("admin", "admin123") == 0 && sys_verify("student", "student123") == 0 && sys_verify("guest", "guest") == 0);
    CHECK(sys_verify("root", "root124") == SYS_EAUTH && sys_verify("root", "") == SYS_EAUTH && sys_verify("root", "ROOT123") == SYS_EAUTH && sys_verify("ghost", "x") == SYS_ENOUSER);
    CHECK(sys_verify("root", "student123") == SYS_EAUTH);                                              /* each account has its own password */

    puts("password hashing");
    uint8_t s1[8] = {1,2,3,4,5,6,7,8}, s2[8] = {1,2,3,4,5,6,7,9}, h1[32], h2[32], h3[32];
    users_hash_password(s1, "secret", h1); users_hash_password(s1, "secret", h2); users_hash_password(s2, "secret", h3);
    CHECK(!memcmp(h1, h2, 32) && memcmp(h1, h3, 32) != 0);                                            /* deterministic; the salt changes the hash */
    users_hash_password(s1, "secreT", h3); CHECK(memcmp(h1, h3, 32) != 0);
    uint8_t a[8], b[8]; users_make_salt("x", a); users_make_salt("x", b); CHECK(memcmp(a, b, 8) != 0);   /* never the same salt twice */
    /* PBKDF-style iteration really happened: it is NOT a single SHA-256 of salt||password */
    uint8_t plain[32], msg[14]; memcpy(msg, s1, 8); memcpy(msg + 8, "secret", 6);
    extern void sha256(const void *, size_t, uint8_t *); sha256(msg, 14, plain); CHECK(memcmp(plain, h1, 32) != 0);
}

static void test_login(void) {
    puts("login, logout and the session");
    cred_info_t ci; user_info_t u;
    CHECK(sys_login("student", "wrong") == SYS_EAUTH && sys_login("nobody", "x") == SYS_EAUTH && sys_login("", "") == SYS_EAUTH && sys_login("ghost", "ghost") == SYS_EAUTH);
    CHECK(sys_getuid() == SYS_ENOUSER);
    CHECK(sys_login("student", "student123") == 0 && sys_getuid() == 2);
    CHECK(sys_getcred(&ci) == 0 && ci.uid == 2 && ci.euid == 2 && ci.gid == 2 && ci.egid == 2 && ci.depth == 1 && ci.sudo == 0 && ci.umask == 022 && !strcmp(ci.name, "student") && !strcmp(ci.ename, "student") && ci.login_tick == 500);
    uint16_t fu, fg; fs_get_user(&fu, &fg); CHECK(fu == 2 && fg == 2);                                  /* MiniFS now checks permissions as student */
    CHECK(sys_user_lookup("student", 0, &u) == 0 && u.logins == 1 && u.last_login == 500 && u.failures == 0);
    CHECK(sys_login("guest", "guest") == 0 && sys_getuid() == 3 && sys_getcred(&ci) == 0 && ci.depth == 1);   /* login replaces the session */
    CHECK(sys_login("student", "bad") == SYS_EAUTH && sys_getuid() == 3);                              /* a failed login leaves the session alone */
    CHECK(sys_logout() == 0 && sys_getuid() == SYS_ENOUSER && sys_logout() == SYS_ENOUSER);
    fs_get_user(&fu, &fg); CHECK(fu == UID_NOBODY);

    puts("failure counter");
    CHECK(sys_user_lookup("student", 0, &u) == 0); uint32_t f0 = u.failures;
    for (int i = 0; i < 3; i++) CHECK(sys_login("student", "nope") == SYS_EAUTH);
    CHECK(sys_user_lookup("student", 0, &u) == 0 && u.failures == f0 + 3);
    CHECK(sys_login("student", "student123") == 0 && sys_user_lookup("student", 0, &u) == 0 && u.failures == 0 && u.logins == 2);
    sys_logout();

    puts("argument validation");
    CHECK(sys_login(NULL, "x") == SYS_EFAULT && sys_login("x", NULL) == SYS_EFAULT && sys_login("abcdefghijklmnop", "x") == SYS_ENAMETOOLONG);
    CHECK(sys_login("student", "0123456789012345678901234567890123456789") == SYS_EINVAL);
    CHECK(sys_getcred(NULL) == SYS_EFAULT && sys_audit_get(0, (audit_event_t *)(uintptr_t)1) == SYS_EFAULT);
}

static void test_lock_su_sudo(void) {
    puts("locked accounts");
    user_info_t u;
    login_as("root", "root123");
    CHECK(sys_user_mod("guest", USER_MOD_LOCK, NULL) == 0 && sys_user_lookup("guest", 0, &u) == 0 && (u.flags & USER_F_LOCKED));
    CHECK(sys_user_mod("root", USER_MOD_LOCK, NULL) == SYS_EPERM && sys_user_mod("ghost", USER_MOD_LOCK, NULL) == SYS_ENOUSER && sys_user_mod("guest", 99, NULL) == SYS_EINVAL);
    CHECK(sys_user_mod("root", USER_MOD_SUDO_OFF, NULL) == SYS_EPERM);
    sys_logout();
    CHECK(sys_login("guest", "guest") == SYS_ELOCKED && sys_login("guest", "wrong") == SYS_EAUTH);      /* a stranger learns nothing about the lock */
    login_as("root", "root123");
    CHECK(sys_su("guest", "") == SYS_ELOCKED);                                                          /* even root cannot become a locked account */
    CHECK(sys_user_mod("guest", USER_MOD_UNLOCK, NULL) == 0 && sys_login("guest", "guest") == 0);
    CHECK(sys_user_mod("guest", USER_MOD_LOCK, NULL) == SYS_EACCES);                                    /* guest is not an administrator */
    sys_logout();
    login_as("root", "root123"); login_as("student", "student123");
    CHECK(sys_user_mod("student", USER_MOD_LOCK, NULL) == SYS_EACCES);
    sys_logout();
    login_as("root", "root123");
    CHECK(sys_su("student", "") == 0 && sys_user_mod("student", USER_MOD_LOCK, NULL) == SYS_EACCES);      /* su student drops root rights */
    sys_logout(); sys_logout();

    puts("su: stacked sessions");
    cred_info_t ci; uint16_t fu, fg;
    CHECK(sys_su("root", "root123") == SYS_ENOUSER);                                                     /* nobody is logged in */
    login_as("student", "student123");
    sys_umask(077);
    CHECK(sys_su("root", "bad") == SYS_EAUTH && sys_su("ghost", "x") == SYS_ENOUSER && sys_getuid() == 2);
    CHECK(sys_su("root", "root123") == 0 && sys_getuid() == 0 && sys_getcred(&ci) == 0 && ci.depth == 2 && ci.umask == 022);
    fs_get_user(&fu, &fg); CHECK(fu == 0);
    CHECK(sys_logout() == 0 && sys_getuid() == 2 && sys_getcred(&ci) == 0 && ci.depth == 1);
    fs_get_user(&fu, &fg); CHECK(fu == 2);
    CHECK(sys_su("guest", "guest") == 0 && sys_getuid() == 3 && sys_logout() == 0);
    sys_logout();
    login_as("root", "root123");
    CHECK(sys_su("student", "") == 0 && sys_getuid() == 2);                                              /* a real root session needs no password */
    CHECK(sys_su("guest", "") == SYS_EAUTH);                                                             /* ...but student does */
    CHECK(sys_su("admin", "admin123") == 0 && sys_su("guest", "guest") == 0);                            /* depth 4 of 4 */
    CHECK(sys_su("student", "student123") == SYS_ELIMIT);
    while (sys_logout() == 0) {}
    CHECK(sys_getuid() == SYS_ENOUSER);

    puts("sudo: one command as root");
    login_as("student", "student123");
    CHECK(sys_sudo_begin("student123") == SYS_EACCES && sys_getcred(&ci) == 0 && ci.euid == 2);          /* not in the sudoers list */
    sys_logout();
    CHECK(sys_sudo_begin("x") == SYS_ENOUSER && sys_sudo_end() == SYS_ESTATE);
    login_as("admin", "admin123");
    CHECK(sys_sudo_begin("wrong") == SYS_EAUTH && sys_sudo_begin("root123") == SYS_EAUTH && sys_getcred(&ci) == 0 && ci.euid == 1);   /* it asks for YOUR password */
    CHECK(sys_user_add("tmpuser", "tmp12345", 0) == SYS_EACCES);
    CHECK(sys_sudo_begin("admin123") == 0 && sys_getcred(&ci) == 0 && ci.sudo == 1 && ci.euid == 0 && ci.egid == 0 && ci.uid == 1 && !strcmp(ci.ename, "root") && !strcmp(ci.name, "admin"));
    fs_get_user(&fu, &fg); CHECK(fu == 0 && fg == 0);
    CHECK(sys_sudo_begin("admin123") == SYS_EBUSY);
    CHECK(sys_user_add("tmpuser", "tmp12345", 0) >= 1000);
    CHECK(sys_su("student", "") == SYS_EAUTH);                                                           /* sudo is not a root login: su still asks */
    CHECK(sys_sudo_end() == 0 && sys_getcred(&ci) == 0 && ci.euid == 1 && sys_sudo_end() == SYS_ESTATE);
    CHECK(sys_user_del("tmpuser", false) == SYS_EACCES);
    sys_sudo_begin("admin123"); sys_logout();                                                            /* logging out ends sudo too */
    CHECK(sys_getcred(&ci) == 0 && ci.sudo == 0 && ci.depth == 0);
    login_as("root", "root123");
    CHECK(sys_user_mod("student", USER_MOD_SUDO_ON, NULL) == 0); sys_logout();
    login_as("student", "student123"); CHECK(sys_sudo_begin("student123") == 0 && sys_sudo_end() == 0); sys_logout();
    login_as("root", "root123"); CHECK(sys_user_mod("student", USER_MOD_SUDO_OFF, NULL) == 0); sys_logout();
    login_as("student", "student123"); CHECK(sys_sudo_begin("student123") == SYS_EACCES);
    sys_logout();
}

static void test_admin(void) {
    puts("creating and deleting accounts");
    user_info_t u; fs_stat_t st; int uid;
    login_as("student", "student123");
    CHECK(sys_user_add("eve", "eve12345", 0) == SYS_EACCES && sys_user_del("guest", false) == SYS_EACCES && sys_user_mod("guest", USER_MOD_COMMENT, "x") == SYS_EACCES);
    sys_logout(); login_as("root", "root123");
    uid = sys_user_add("alice", "alice123", 0);
    CHECK(uid == 1000 && sys_user_lookup("alice", 0, &u) == 0 && u.uid == 1000 && u.gid == 1000 && !strcmp(u.home, "/home/alice"));
    CHECK(sys_stat("/home/alice", &st) == 0 && st.type == FS_DIR && st.uid == 1000 && st.gid == 1000 && st.mode == 0750);
    CHECK(sys_verify("alice", "alice123") == 0 && sys_verify("alice", "alice124") == SYS_EAUTH);
    CHECK(file_text_is("/etc/passwd", "alice:x:1000:1000:User account:/home/alice:active"));
    CHECK(sys_user_add("alice", "other123", 0) == SYS_EEXIST && sys_user_add("root", "other123", 0) == SYS_EEXIST);
    CHECK(sys_user_add("Bad", "pass1234", 0) == SYS_EINVAL && sys_user_add("9lives", "pass1234", 0) == SYS_EINVAL && sys_user_add("", "pass1234", 0) == SYS_EINVAL &&
          sys_user_add("a b", "pass1234", 0) == SYS_EINVAL && sys_user_add("a/b", "pass1234", 0) == SYS_EINVAL && sys_user_add("nobody", "pass1234", 0) == SYS_EINVAL);
    CHECK(sys_user_add("abcdefghijklmnop", "pass1234", 0) == SYS_ENAMETOOLONG && sys_user_add("abcdefghijklmno", "pass1234", 0) > 0);   /* 15 characters fit */
    sys_user_del("abcdefghijklmno", true);
    CHECK(sys_user_add("bob", "abc", 0) == SYS_EINVAL && sys_user_add("bob", "012345678901234567890123456789012", 0) == SYS_EINVAL &&   /* 33 characters */
          sys_user_add("bob", "0123456789012345678901234567890123456789", 0) == SYS_EINVAL && sys_user_add("bob", "tab\there1", 0) == SYS_EINVAL && sys_user_add("bob", "pass1234", 2) == SYS_EINVAL);
    CHECK(sys_user_add("bob", "01234567890123456789012345678901", 0) > 0 && sys_verify("bob", "01234567890123456789012345678901") == 0);   /* 32 characters: the maximum */
    CHECK(sys_user_add("carl", "pass1234", USER_F_SUDO) > 0 && sys_user_lookup("carl", 0, &u) == 0 && (u.flags & USER_F_SUDO));
    CHECK(sys_user_add("dora", "pass1234", 0) > 0);
    CHECK(sys_user_add("erin", "pass1234", 0) == SYS_ELIMIT);                                           /* 4 built-in + 4 = USER_MAX (8) */
    CHECK(sys_user_get(7, &u) == 0 && sys_user_get(8, &u) == SYS_ENOENT);

    puts("deleting");
    CHECK(sys_user_del("root", false) == SYS_EPERM && sys_user_del("ghost", false) == SYS_ENOUSER);
    CHECK(sys_user_del("dora", false) == 0 && sys_stat("/home/dora", &st) == 0 && sys_user_lookup("dora", 0, &u) == SYS_ENOUSER);   /* the home directory stays */
    CHECK(sys_stat("/home/dora", &st) == 0 && st.uid == 1004);                                          /* ...owned by a uid nobody has any more */
    CHECK(sys_user_del("carl", true) == 0 && sys_stat("/home/carl", &st) == SYS_ENOENT && !file_text_is("/etc/passwd", "carl:"));
    CHECK(sys_user_add("erin", "pass1234", 0) == 1005);                                                 /* uids are never reused */
    CHECK(sys_chown("/home/dora", 1004, UID_KEEP) == SYS_ENOUSER);                                      /* cannot hand files to a deleted uid */
    CHECK(sys_user_del("root", true) == SYS_EPERM);
    sys_logout();
    login_as("alice", "alice123");
    CHECK(sys_user_del("alice", false) == SYS_EACCES);
    sys_logout(); login_as("root", "root123");
    CHECK(sys_su("alice", "") == 0 && sys_user_del("alice", false) == SYS_EACCES);                       /* root-in-su-alice is just alice */
    sys_logout();
    CHECK(sys_su("bob", "") == 0); sys_logout();
    sys_logout();
    login_as("alice", "alice123"); sys_logout(); login_as("root", "root123");
    CHECK(sys_su("alice", "") == 0); /* alice is a live session under root: root cannot delete alice while she is logged in */
    sys_logout();

    puts("the /etc/passwd mirror");
    sys_user_mod("alice", USER_MOD_COMMENT, "Alice the tester");
    CHECK(file_text_is("/etc/passwd", "alice:x:1000:1000:Alice the tester:/home/alice:active"));
    CHECK(sys_user_mod("alice", USER_MOD_COMMENT, "has:colon") == SYS_EINVAL && sys_user_mod("alice", USER_MOD_COMMENT, "") == SYS_EINVAL &&
          sys_user_mod("alice", USER_MOD_COMMENT, "0123456789012345678901234567890123") == SYS_EINVAL);
    sys_user_mod("alice", USER_MOD_LOCK, NULL); CHECK(file_text_is("/etc/passwd", "alice:x:1000:1000:Alice the tester:/home/alice:locked"));
    sys_user_mod("alice", USER_MOD_UNLOCK, NULL);
    {   /* no hash, salt or password text ever reaches the file */
        char b[1024]; int fd = sys_open("/etc/passwd", SYS_O_READ); int n = fd >= 0 ? sys_read(fd, b, sizeof b - 1) : -1; if (fd >= 0) sys_close(fd);
        CHECK(n > 0); b[n > 0 ? n : 0] = 0;
        int run_hex = 0, longest = 0; for (int i = 0; i < n; i++) { char c = b[i]; run_hex = ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) ? run_hex + 1 : 0; if (run_hex > longest) longest = run_hex; }
        CHECK(longest < 20 && !strstr(b, "root123") && !strstr(b, "alice123") && !strstr(b, "student123"));
    }
    sys_logout();
    login_as("student", "student123");
    CHECK(write_file("/etc/passwd", "root::0:0::/:") == SYS_EACCES);                                    /* students cannot rewrite the account list */
    CHECK(file_text_is("/etc/passwd", "student:x:2:2"));                                                /* but may read it */
    sys_logout();
}

static void test_passwd(void) {
    puts("changing passwords");
    login_as("student", "student123");
    CHECK(sys_user_passwd("student", "bad", "newpass1") == SYS_EAUTH && sys_user_passwd("student", NULL, "newpass1") == SYS_EAUTH);
    CHECK(sys_user_passwd("student", "student123", "abc") == SYS_EINVAL && sys_verify("student", "student123") == 0);
    CHECK(sys_user_passwd("guest", "guest", "hacked12") == SYS_EACCES && sys_verify("guest", "guest") == 0);   /* cannot touch someone else's */
    CHECK(sys_user_passwd("ghost", "x", "newpass1") == SYS_ENOUSER);
    CHECK(sys_user_passwd("student", "student123", "newpass1") == 0);
    CHECK(sys_verify("student", "student123") == SYS_EAUTH && sys_verify("student", "newpass1") == 0);
    sys_logout();
    CHECK(sys_login("student", "student123") == SYS_EAUTH && sys_login("student", "newpass1") == 0);
    sys_logout(); login_as("root", "root123");
    CHECK(sys_user_passwd("guest", NULL, "reset1234") == 0 && sys_verify("guest", "reset1234") == 0 && sys_verify("guest", "guest") == SYS_EAUTH);   /* root needs no old password */
    CHECK(sys_user_passwd("guest", NULL, "xyz") == SYS_EINVAL);
    sys_logout();
    puts("sudo may reset passwords too");
    login_as("admin", "admin123");
    CHECK(sys_user_passwd("guest", NULL, "again1234") == SYS_EACCES);
    CHECK(sys_sudo_begin("admin123") == 0 && sys_user_passwd("guest", NULL, "again1234") == 0 && sys_sudo_end() == 0 && sys_verify("guest", "again1234") == 0);
    sys_logout();
}

static void test_ownership(void) {
    puts("chown and chgrp");
    login_as("root", "root123");
    CHECK(sys_create("/tmp/f") == 0 && owner_of("/tmp/f") == 0);
    CHECK(sys_chown("/tmp/f", 2, 3) == 0 && owner_of("/tmp/f") == 2 && group_of("/tmp/f") == 3);
    CHECK(sys_chown("/tmp/f", UID_KEEP, 1) == 0 && owner_of("/tmp/f") == 2 && group_of("/tmp/f") == 1);
    CHECK(sys_chown("/tmp/f", 3, UID_KEEP) == 0 && owner_of("/tmp/f") == 3 && group_of("/tmp/f") == 1);
    CHECK(sys_chown("/tmp/f", 77, UID_KEEP) == SYS_ENOUSER && sys_chown("/tmp/f", UID_KEEP, 77) == SYS_ENOUSER && sys_chown("/tmp/f", 70000, 0) == SYS_ENOUSER);
    CHECK(sys_chown("/tmp/missing", 1, 1) == SYS_ENOENT && sys_chown(NULL, 1, 1) == SYS_EFAULT && owner_of("/tmp/f") == 3);
    sys_logout();
    login_as("student", "student123");
    CHECK(sys_create("/tmp/mine") == 0 && owner_of("/tmp/mine") == 2 && group_of("/tmp/mine") == 2);
    CHECK(sys_chown("/tmp/mine", 3, 3) == SYS_EACCES && sys_chown("/tmp/mine", 2, 2) == SYS_EACCES && owner_of("/tmp/mine") == 2);   /* nobody gives files away but root */
    sys_logout();
}

static void test_permissions(void) {
    puts("permissions are enforced for the logged-in user");
    login_as("root", "root123");
    CHECK(write_file("/root/secret", "top secret\n") == 0); sys_logout();
    login_as("student", "student123");
    CHECK(sys_open("/root/secret", SYS_O_READ) == SYS_EACCES && sys_chdir("/root") == SYS_EACCES && sys_chdir("/home/admin") == SYS_EACCES);
    CHECK(write_file("/tmp/shared.txt", "hello from student\n") == 0 && mode_of("/tmp/shared.txt") == 0644 && owner_of("/tmp/shared.txt") == 2);
    CHECK(sys_mkdir("/etc/newdir", 0) == SYS_EACCES && sys_create("/etc/newfile") == SYS_EACCES && write_file("/etc/hostname", "hacked\n") == SYS_EACCES);
    CHECK(sys_chdir("/home/student") == 0 && write_file("notes.txt", "my notes\n") == 0);
    sys_logout();
    login_as("guest", "guest");
    CHECK(sys_open("/tmp/shared.txt", SYS_O_READ) >= 3);                                                /* others may read a 0644 file */
    sys_close(3);
    CHECK(sys_open("/tmp/shared.txt", SYS_O_WRITE) == SYS_EACCES && sys_chmod("/tmp/shared.txt", 0777) == SYS_EACCES);
    CHECK(sys_chdir("/home/student") == SYS_EACCES && sys_open("/home/student/notes.txt", SYS_O_READ) == SYS_EACCES);   /* /home/student is 0750 */
    sys_logout();
    login_as("student", "student123");
    CHECK(sys_chmod("/tmp/shared.txt", 0600) == 0 && mode_of("/tmp/shared.txt") == 0600);
    sys_logout(); login_as("guest", "guest");
    CHECK(sys_open("/tmp/shared.txt", SYS_O_READ) == SYS_EACCES);                                        /* now closed to everyone else */
    sys_logout(); login_as("student", "student123");
    CHECK(sys_open("/tmp/shared.txt", SYS_O_READ) >= 3); sys_close(3);                                   /* the owner still can */
    sys_logout(); login_as("root", "root123");
    CHECK(sys_open("/tmp/shared.txt", SYS_O_READ) >= 3 && sys_chdir("/home/student") == 0);              /* root can open anything */
    sys_close(3); sys_chdir("/");
    sys_logout();
    puts("a deleted user's files are not inherited by the next account");
    login_as("root", "root123"); sys_create("/tmp/f");
    int old = sys_user_add("temp1", "pass1234", 0); CHECK(old > 0);
    CHECK(sys_chown("/tmp/f", (uint32_t)old, UID_KEEP) == 0);
    CHECK(sys_user_del("temp1", false) == 0);
    int fresh_uid = sys_user_add("temp2", "pass1234", 0); CHECK(fresh_uid > old && owner_of("/tmp/f") == old);
    sys_logout();
}

static void test_umask(void) {
    puts("umask");
    login_as("student", "student123");
    CHECK(sys_umask(-1) == 022);
    CHECK(sys_create("/tmp/u1") == 0 && mode_of("/tmp/u1") == 0644 && sys_mkdir("/tmp/d1", 0) == 0 && mode_of("/tmp/d1") == 0755);
    CHECK(sys_umask(077) == 022 && sys_umask(-1) == 077);
    CHECK(sys_create("/tmp/u2") == 0 && mode_of("/tmp/u2") == 0600 && sys_mkdir("/tmp/d2", 0) == 0 && mode_of("/tmp/d2") == 0700);
    CHECK(sh("echo hi > /tmp/u3") == 0 && mode_of("/tmp/u3") == 0600);                                  /* redirection obeys it too */
    CHECK(sys_umask(0777) == 077 && sys_create("/tmp/u4") == 0 && mode_of("/tmp/u4") == 0000);
    CHECK(sys_umask(01000) == SYS_EINVAL && sys_umask(-1) == 0777);
    sys_logout(); login_as("student", "student123");
    CHECK(sys_umask(-1) == 022);                                                                         /* every login starts with the default */
    sys_logout();
    puts("the standard layout does not depend on the umask");
    login_as("root", "root123");
    sys_umask(0777);
    CHECK(sh("mkfs --yes") == 0 && mode_of("/") == 0755 && mode_of("/etc") == 0755 && sys_umask(-1) == 0777);
    sys_logout();
}

static void test_audit(void) {
    puts("audit trail");
    audit_event_t e;
    CHECK(event_count() == 0 && sys_audit_get(0, &e) == SYS_ENOENT);
    sys_login("student", "TopSecret99"); sys_login("student", "student123"); sys_logout();
    CHECK(event_count() == 3);
    CHECK(sys_audit_get(0, &e) == 0 && e.type == AUDIT_LOGIN_FAIL && e.result == SYS_EAUTH && e.uid == (int)UID_NOBODY && !strcmp(e.subject, "student") && e.tick == 500);
    CHECK(sys_audit_get(1, &e) == 0 && e.type == AUDIT_LOGIN && e.result == 0 && e.uid == 2);
    CHECK(sys_audit_get(2, &e) == 0 && e.type == AUDIT_LOGOUT && e.uid == 2);
    CHECK(sys_audit_get(3, &e) == SYS_ENOENT);
    login_as("student", "student123"); sys_user_add("x", "pass1234", 0);
    CHECK(sys_audit_get(3, &e) == 0 && e.type == AUDIT_LOGIN && sys_audit_get(4, &e) == 0 && e.type == AUDIT_DENIED && e.result == SYS_EACCES);
    sys_logout();
    bool leaked = false;
    for (int i = 0; i < event_count(); i++) if (sys_audit_get((uint32_t)i, &e) == 0 && (strstr(e.subject, "TopSecret99") || strstr(e.subject, "student123"))) leaked = true;
    CHECK(!leaked);                                                                                      /* passwords are never logged */
    puts("the ring buffer keeps the newest events");
    for (int i = 0; i < 40; i++) sys_login("guest", "nope");
    CHECK(event_count() == AUDIT_MAX);
    CHECK(sys_audit_get(AUDIT_MAX - 1, &e) == 0 && e.type == AUDIT_LOGIN_FAIL && !strcmp(e.subject, "guest") && sys_audit_get(AUDIT_MAX, &e) == SYS_ENOENT);
    CHECK(sys_audit_get(0, &e) == 0 && e.type == AUDIT_LOGIN_FAIL);                                      /* the old events scrolled away */
    CHECK(!strcmp(audit_event_name(AUDIT_SUDO), "sudo") && !strcmp(audit_event_name(999), "?"));
}

static void test_leaks(void) {
    puts("no memory or descriptor leaks");
    login_as("root", "root123");
    sys_user_add("warm", "pass1234", 0); sys_user_del("warm", true); sys_logout();                        /* warm-up: grows the passwd file buffer once */
    heap_stats_t h0, h1; heap_stats(&h0);
    for (int i = 0; i < 5; i++) {
        sys_login("student", "wrong"); login_as("root", "root123");
        sys_su("student", ""); sys_logout();
        sys_user_add("cycle", "pass1234", 0); sys_user_passwd("cycle", NULL, "other1234"); sys_user_mod("cycle", USER_MOD_COMMENT, "temp"); sys_user_del("cycle", true);
        sys_logout();
        login_as("admin", "admin123"); sys_sudo_begin("admin123"); sys_sudo_end(); sys_logout();
    }
    heap_stats(&h1);
    CHECK(h1.used_blocks == h0.used_blocks && h1.payload_used == h0.payload_used);
    CHECK(heap_check(NULL) == HEAP_OK);
    fs_check_report_t r; CHECK(fs_check(&r) == 0);
}

static void test_commands(void) {
    puts("commands: not logged in");
    CHECK(sh("whoami") == 1 && has("nobody is logged in") && sh("id") == 1 && sh("groups") == 1 && sh("logout") == 1 && sh("sessions") == 0 && has("(nobody)") && sh("who") == 0 && has("nobody is logged in"));

    puts("commands: login, whoami, id, users");
    feed("student123\n");
    CHECK(sh("login student") == 0 && has("Logged in as student") && has("Password: **********") && !has("student123") && fed_all());   /* typed text shows only as stars */
    CHECK(sh("pwd") == 0 && !strcmp(out, "/home/student\n"));                                              /* login starts in the home directory */
    CHECK(sh("whoami") == 0 && !strcmp(out, "student\n") && sh("groups") == 0 && !strcmp(out, "student\n") && sh("homedir") == 0 && !strcmp(out, "/home/student\n"));
    CHECK(sh("id") == 0 && has("uid=2(student) gid=2(student)") && sh("id admin") == 0 && has("uid=1(admin)") && sh("id ghost") == 1 && has("user 'ghost' does not exist"));
    CHECK(sh("users") == 0 && has("root") && has("admin") && has("student") && has("guest") && has("* ") && has("sudo") && has("salted SHA-256"));
    CHECK(sh("userinfo guest") == 0 && has("UID / GID     : 3 / 3") && has("/home/guest") && sh("userinfo") == 1 && sh("userinfo ghost") == 1);
    CHECK(sh("who") == 0 && has("student") && sh("sessions") == 0 && has("Umask           : 0022") && has("Logged in as    : student"));
    feed("wrong\n");
    CHECK(sh("login admin") == 1 && has("login incorrect") && fed_all() && sh("whoami") == 0 && !strcmp(out, "student\n"));   /* failure keeps the session */
    feed("x\n"); CHECK(sh("login ghost") == 1 && has("login incorrect") && sh("login") == 1 && has("usage:"));
    kq[0] = 0; CHECK(sh("login student") == 1 && has("cancelled"));                                       /* Ctrl+C at the prompt */

    puts("commands: ordinary users cannot administer");
    CHECK(sh("useradd bob") == 1 && has("permission denied") && sh("userdel guest") == 1 && has("permission denied") && sh("lockuser guest") == 1 && sh("usermod guest sudo on") == 1);
    CHECK(sh("chown admin /tmp") == 1 && has("only root may change ownership") && sh("chgrp admin /tmp") == 1);
    CHECK(sh("mkfs --yes") == 1 && has("only root may erase"));
    CHECK(sh("sh_nothing") == 127);
    feed("student123\n"); CHECK(sh("sudo mkdir /etc/x") == 1 && has("not allowed to use sudo") && fed_all() && sh("stat /etc/x") == 1);
    CHECK(sh("sudo") == 1 && has("usage:") && sh("sudo nosuchcmd") == 127 && sh("sudo sudo ls") == 1 && has("cannot be run through sudo") && sh("sudo logout") == 1 && sh("sudo su") == 1 && sh("sudo login x") == 1);

    puts("commands: umask, access, perms");
    CHECK(sh("umask") == 0 && !strcmp(out, "0022\n") && sh("umask 077") == 0 && has("mode 600") && has("new directories 700"));
    CHECK(sh("umask") == 0 && !strcmp(out, "0077\n") && sh("touch /tmp/m1") == 0 && mode_of("/tmp/m1") == 0600);
    CHECK(sh("umask 9") == 1 && sh("umask 07777") == 1 && sh("umask abc") == 1 && sh("umask 1 2") == 1 && sh("umask 022") == 0 && sh("umask") == 0 && !strcmp(out, "0022\n"));
    CHECK(sh("touch /tmp/p1") == 0 && sh("perms /tmp/p1") == 0 && has("-rw-r--r--") && has("0644") && has("student:student") && sh("perms /nope") == 1 && has("does not exist"));
    CHECK(sh("access guest /tmp/p1") == 0 && has("read=yes write=no execute=no") && sh("access student /tmp/p1") == 0 && has("read=yes write=yes execute=no") &&
          sh("access root /tmp/p1") == 0 && has("read=yes write=yes execute=yes") && sh("chmod 600 /tmp/p1") == 0 && sh("access guest /tmp/p1") == 0 && has("read=no write=no execute=no"));
    CHECK(sh("access ghost /tmp/p1") == 1 && has("does not exist") && sh("access guest /nope") == 1 && sh("access guest") == 1 && has("usage:"));
    CHECK(sh("whoowns /tmp/p1 /etc/motd") == 0 && has("/tmp/p1: student:student") && has("/etc/motd: root:root") && sh("whoowns") == 1);
    CHECK(sh("logout") == 0 && has("Logged out.") && sh("whoami") == 1);

    puts("commands: sudo as admin");
    feed("admin123\n"); CHECK(sh("login admin") == 0 && fed_all());
    feed("admin123\n"); CHECK(sh("sudo mkdir /etc/x") == 0 && fed_all() && owner_of("/etc/x") == 0);        /* really created, really owned by root */
    CHECK(sh("whoami") == 0 && !strcmp(out, "admin\n") && sh("mkdir /etc/y") == 1 && has("permission denied"));   /* rights dropped again */
    feed("bad\n"); CHECK(sh("sudo mkdir /etc/z") == 1 && has("incorrect password") && sh("stat /etc/z") == 1);
    feed("admin123\n"); CHECK(sh("sudo rm /nonexistent") == 1 && has("not found") && sh("mkdir /etc/y") == 1);   /* a failing command still drops root */
    feed("admin123\n"); CHECK(sh("sudo whoami") == 0 && !strcmp(out, "[sudo] password for admin: ********\nroot\n"));
    feed("admin123\n"); CHECK(sh("sudo mkfs") == 1 && has("ERASES") && sh("stat /home/admin") == 0);       /* mkfs asks for confirmation first */

    puts("commands: useradd, passwd, lock, usermod (through sudo)");
    feed("admin123\nPw123456\nPw123456\n"); CHECK(sh("sudo useradd -s carol") == 0 && fed_all() && has("Created user 'carol' (uid 1000)") && has("home /home/carol"));
    CHECK(sys_verify("carol", "Pw123456") == 0 && file_text_is("/etc/passwd", "carol:x:1000:1000"));
    feed("admin123\nabcd1\nabcd2\n"); CHECK(sh("sudo useradd dave") == 1 && has("do not match") && fed_all()); { user_info_t u; CHECK(sys_user_lookup("dave", 0, &u) == SYS_ENOUSER); }
    feed("admin123\nPw123456\nPw123456\n"); CHECK(sh("sudo useradd Bad!") == 1 && has("invalid name or password") && fed_all());
    feed("admin123\nPw123456\nPw123456\n"); CHECK(sh("sudo useradd carol") == 1 && has("already exists") && fed_all());
    CHECK(sh("useradd") == 1 && has("usage:") && sh("useradd -x") == 1 && sh("useradd a b") == 1);
    feed("admin123\n"); CHECK(sh("sudo lockuser carol") == 0 && has("locked") && sh("users") == 0 && has("locked"));
    feed("admin123\n"); CHECK(sh("sudo usermod carol sudo off") == 0 && has("may no longer use sudo"));
    feed("admin123\n"); CHECK(sh("sudo usermod carol comment \"Carol C\"") == 0 && sh("userinfo carol") == 0 && has("Carol C") && has("locked"));
    feed("admin123\n"); CHECK(sh("sudo usermod carol sudo maybe") == 1 && has("usage:") && sh("usermod") == 1);
    feed("admin123\n"); CHECK(sh("sudo unlockuser carol") == 0 && has("unlocked"));
    feed("admin123\n"); CHECK(sh("sudo userdel -r carol") == 0 && has("and the home directory") && sh("stat /home/carol") == 1);
    feed("admin123\n"); CHECK(sh("sudo userdel carol") == 1 && has("user 'carol' does not exist"));
    feed("admin123\n"); CHECK(sh("sudo userdel admin") == 1 && has("busy"));                              /* cannot delete the account you are using */

    puts("commands: passwd");
    feed("admin123\nnewpass9\nnewpass9\n"); CHECK(sh("passwd") == 0 && fed_all() && has("Password updated for admin"));
    feed("admin123\nnewpass8\nnewpass8\n"); CHECK(sh("passwd") == 1 && has("current password is wrong") && sys_verify("admin", "newpass9") == 0);
    feed("newpass9\nabc\nabc\n"); CHECK(sh("passwd") == 1 && has("invalid"));
    feed("newpass9\nnewpass7\nnewpass6\n"); CHECK(sh("passwd") == 1 && has("do not match") && sys_verify("admin", "newpass9") == 0);
    feed("newpass9\nnewpass7\nnewpass7\n"); CHECK(sh("passwd admin") == 0);
    feed("x\nnewpass6\nnewpass6\n"); CHECK(sh("passwd guest") == 1 && has("permission denied"));        /* not yours */
    feed("newpass7\nzzzzzz\nzzzzzz\n"); CHECK(sh("sudo passwd guest") == 0);                              /* sudo: root needs no old password... */
    CHECK(sys_verify("guest", "zzzzzz") == 0);
    feed("newpass7\n"); CHECK(sh("sudo passwd") == 1 && fed_all());                              /* ...but the prompts run out: cancelled, nothing changes */
    kq[0] = 0; CHECK(sys_verify("admin", "newpass7") == 0);

    puts("commands: su and logout");
    feed("root123\n"); CHECK(sh("su") == 0 && has("Now acting as root") && sh("whoami") == 0 && !strcmp(out, "root\n"));
    CHECK(sh("id") == 0 && has("uid=0(root)") && sh("sessions") == 0 && has("Nested su depth : 2 of 4"));
    CHECK(sh("su student") == 0 && sh("whoami") == 0 && !strcmp(out, "student\n"));                       /* root needs no password */
    CHECK(sh("logout") == 0 && has("Back to root") && sh("logout") == 0 && has("Back to admin") && sh("whoami") == 0 && !strcmp(out, "admin\n"));
    feed("x\n"); CHECK(sh("su ghost") == 1 && has("does not exist")); kq[0] = 0;
    feed("nope\n"); CHECK(sh("su guest") == 1 && has("authentication failed") && sh("su a b") == 1 && has("usage:"));
    feed("root123\n"); CHECK(sh("su") == 0); kq[0] = 0;

    puts("commands: ownership as root");
    CHECK(sh("touch /tmp/o1") == 0 && sh("chown student /tmp/o1") == 0 && sh("whoowns /tmp/o1") == 0 && has("student:root"));
    CHECK(sh("chgrp guest /tmp/o1") == 0 && has("") && sh("whoowns /tmp/o1") == 0 && has("student:guest"));
    CHECK(sh("chown admin:admin /tmp/o1") == 0 && sh("whoowns /tmp/o1") == 0 && has("admin:admin"));
    CHECK(sh("chown :student /tmp/o1") == 0 && sh("whoowns /tmp/o1") == 0 && has("admin:student"));
    CHECK(sh("chown 2 /tmp/o1") == 0 && owner_of("/tmp/o1") == 2 && sh("chown guest:3 /tmp/o1") == 0 && owner_of("/tmp/o1") == 3 && group_of("/tmp/o1") == 3);
    CHECK(sh("chown nosuch /tmp/o1") == 1 && has("user 'nosuch' does not exist") && sh("chgrp nosuch /tmp/o1") == 1 && sh("chown 99 /tmp/o1") == 1);
    CHECK(sh("chown student /tmp/missing") == 1 && has("does not exist") && sh("chown student") == 1 && has("usage:") && sh("chown : /tmp/o1") == 1 && sh("chgrp") == 1);
    CHECK(sh("stat /tmp/o1") == 0 && has("Owner  : guest (uid 3), group guest (gid 3)"));
    CHECK(sh("touch /tmp/a1 /tmp/a2") == 0 && sh("chown student /tmp/a1 /tmp/a2") == 0 && owner_of("/tmp/a1") == 2 && owner_of("/tmp/a2") == 2);

    puts("commands: audit, last, secstatus, motd, hashpw, checkpw");
    sys_login("ghost", "x"); sys_sudo_begin("x");                                                         /* fresh events: the ring only keeps the last 32 */
    CHECK(sh("last 30") == 0 && has("login") && has("login-failed") && has("logout") && sh("last 2") == 0 && sh("last x") == 1 && has("usage:") && sh("last 1 2") == 1);
    CHECK(sh("audit") == 0 && has("sudo-failed") && has("login-failed") && has("EAUTH") && has("EACCES") && sh("audit 3") == 0 && has("ghost"));
    CHECK(sh("secstatus") == 0 && has("salted SHA-256, 1000 rounds, 8-byte salt") && has("NOT provided") && has("Audit events kept"));
    CHECK(sh("motd") == 0 && has("Welcome to MiniOS"));
    feed("MySecretText\n"); CHECK(sh("hashpw") == 0 && has("salt : ") && has("hash : ") && !has("MySecretText") && fed_all());
    { const char *p = strstr(out, "hash : "); CHECK(p != NULL && strspn(p + 7, "0123456789abcdef") == 64); }
    feed("student123\n"); CHECK(sh("checkpw student") == 0 && has("correct"));
    feed("no\n"); CHECK(sh("checkpw student") == 1 && has("incorrect"));
    feed("x\n"); CHECK(sh("checkpw ghost") == 1 && has("does not exist") && sh("checkpw") == 1);

    puts("commands: mkfs as root restores homes and the account list");
    sys_user_add("keeper", "pass1234", 0);
    CHECK(sh("mkfs --yes") == 0);
    { fs_stat_t st; CHECK(sys_stat("/home/guest", &st) == 0 && st.uid == 3 && st.mode == 0750 && sys_stat("/root", &st) == 0 && st.mode == 0700 && sys_stat("/home/keeper", &st) == 0 && st.uid >= 1000);
      CHECK(file_text_is("/etc/passwd", "keeper:x:") && file_text_is("/etc/passwd", "guest:x:3:3")); }
    sys_logout(); while (sys_logout() == 0) {}
    CHECK(sys_getuid() == SYS_ENOUSER);
    CHECK(heap_check(NULL) == HEAP_OK);
    { fs_check_report_t r; CHECK(fs_check(&r) == 0); }
}

static int real_main(void) {
    fresh();
    CHECK(builtins_register() == 0);
    const char *mine[] = { "whoami","users","login","logout","su","sudo","useradd","userdel","passwd","id","groups","who","last","usermod","userinfo",
                           "lockuser","unlockuser","homedir","sessions","motd","chown","chgrp","umask","access","perms","audit","secstatus","hashpw","checkpw","whoowns" };
    for (size_t i = 0; i < sizeof mine / sizeof mine[0]; i++) CHECK(command_find(mine[i]) && command_find(mine[i])->handler);
    test_defaults();  fresh(); builtins_register();
    test_login();     fresh(); builtins_register();
    test_lock_su_sudo(); fresh(); builtins_register();
    test_admin();     fresh(); builtins_register();
    test_passwd();    fresh(); builtins_register();
    test_ownership(); fresh(); builtins_register();
    test_permissions(); fresh(); builtins_register();
    test_umask();     fresh(); builtins_register();
    test_audit();     fresh(); builtins_register();
    test_leaks();     fresh(); builtins_register();
    test_commands();
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}

int main(void) { return run_on_low_stack(real_main); }
