/* User, session and security commands (Phase 10).
 * Everything here goes through system calls (sys_login, sys_user_add, sys_chown, ...): the kernel's user
 * manager decides who may do what. These commands only collect input, print results and explain errors.
 * Passwords are read with cu_read_secret(), never taken from the command line (so they stay out of `history`). */
#include "include/builtins.h"
#include "include/cmdutil.h"
#include "include/screen.h"
#include "include/syscall.h"
#include "include/users.h"
#include "include/sha256.h"
#include "include/timer.h"
#include "include/numparse.h"
#include "include/string.h"

#define PW_BUF (USER_PASS_MAX + 4)

/* ---------- output helpers ---------- */
static void err_begin(const char *cmd) { cu_color(VGA_LIGHT_RED); screen_puts("Error: "); cu_color(VGA_LIGHT_GREY); screen_puts(cmd); screen_puts(": "); }

/* Explains a SYS_E* code in user terms. `name` is the account the command was about (or NULL). */
static int user_error(const char *cmd, const char *name, int rc) {
    err_begin(cmd);
    switch (rc) {
        case SYS_ENOUSER: if (name) { screen_puts("user '"); screen_puts(name); screen_puts("' does not exist.\n"); } else screen_puts("nobody is logged in.\n"); break;
        case SYS_EAUTH:   screen_puts("authentication failed.\n"); break;
        case SYS_ELOCKED: screen_puts("account is locked.\n"); break;
        case SYS_EACCES:  screen_puts("permission denied (administrator action: use 'sudo' or 'su root').\n"); break;
        case SYS_EEXIST:  screen_puts("user '"); screen_puts(name ? name : "?"); screen_puts("' already exists.\n"); break;
        case SYS_EINVAL:  screen_puts("invalid name or password (names: a-z 0-9 _ -, starting with a letter, up to 15; passwords: 4 to 32 printable characters).\n"); break;
        case SYS_ELIMIT:  screen_puts("limit reached (account table or session depth).\n"); break;
        case SYS_EBUSY:   screen_puts("busy: that account is logged in here (or sudo is already active).\n"); break;
        case SYS_EPERM:   screen_puts("not allowed for this account (root is protected).\n"); break;
        default:          screen_puts(sys_strerror(rc)); screen_putc('\n'); break;
    }
    return 1;
}

static void put_octal3(uint32_t m) { screen_put_dec((m >> 6) & 7); screen_put_dec((m >> 3) & 7); screen_put_dec(m & 7); }
static void put_time(uint32_t ticks) { screen_put_dec(ticks / TIMER_HZ); screen_puts(" s"); }

static bool get_cred(const char *cmd, cred_info_t *ci) {
    int rc = sys_getcred(ci);
    if (rc != 0) { cu_error(cmd, sys_strerror(rc)); return false; }
    return true;
}

static void put_flags(uint32_t f) {
    bool any = false;
    if (f & USER_F_SUDO)   { screen_puts("sudo"); any = true; }
    if (f & USER_F_LOCKED) { if (any) screen_putc(','); screen_puts("locked"); any = true; }
    if (!any) screen_putc('-');
}

/* Reads a password twice; false on mismatch or Ctrl+C. */
static bool read_new_password(const char *cmd, char *pw, const char *first_prompt) {
    char again[PW_BUF];
    if (!cu_read_secret(first_prompt, pw, PW_BUF) || !cu_read_secret("Retype new password: ", again, sizeof again)) { cu_wipe(again, sizeof again); cu_error(cmd, "cancelled or too long."); return false; }
    bool same = !strcmp(pw, again);
    cu_wipe(again, sizeof again);
    if (!same) { cu_error(cmd, "passwords do not match; nothing was changed."); return false; }
    return true;
}

/* ---------- who am I ---------- */
static int cmd_whoami(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("whoami");
    cred_info_t ci; if (!get_cred("whoami", &ci)) return 1;
    if (ci.depth == 0) return user_error("whoami", NULL, SYS_ENOUSER);
    screen_puts(ci.ename); screen_putc('\n');
    return 0;
}

static void print_id(const cred_info_t *ci) {
    char n[20];
    screen_puts("uid="); screen_put_dec((uint32_t)ci->uid); screen_putc('('); screen_puts(ci->name); screen_puts(") gid=");
    screen_put_dec((uint32_t)ci->gid); screen_putc('('); cu_uid_name((uint32_t)ci->gid, n, sizeof n); screen_puts(n); screen_putc(')');
    if (ci->sudo) { screen_puts(" euid=0(root) egid=0(root)  [sudo active]"); }
    screen_putc('\n');
}

static int cmd_id(int argc, const char **argv) {
    if (argc > 2) return cu_usage("id [user]");
    if (argc == 1) { cred_info_t ci; if (!get_cred("id", &ci)) return 1; if (ci.depth == 0) return user_error("id", NULL, SYS_ENOUSER); print_id(&ci); return 0; }
    user_info_t u; int rc = sys_user_lookup(argv[1], 0, &u);
    if (rc != 0) return user_error("id", argv[1], rc);
    char g[20]; cu_uid_name(u.gid, g, sizeof g);
    screen_puts("uid="); screen_put_dec(u.uid); screen_putc('('); screen_puts(u.name); screen_puts(") gid="); screen_put_dec(u.gid); screen_putc('('); screen_puts(g); screen_puts(")\n");
    return 0;
}

static int cmd_groups(int argc, const char **argv) {
    if (argc > 2) return cu_usage("groups [user]");
    user_info_t u; int rc;
    if (argc == 2) rc = sys_user_lookup(argv[1], 0, &u);
    else { int uid = sys_getuid(); rc = uid < 0 ? uid : sys_user_lookup(NULL, uid, &u); }
    if (rc != 0) return user_error("groups", argc == 2 ? argv[1] : NULL, rc);
    char g[20]; cu_uid_name(u.gid, g, sizeof g);
    screen_puts(g); screen_putc('\n');
    return 0;
}

static int cmd_users(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("users");
    cred_info_t ci; if (!get_cred("users", &ci)) return 1;
    screen_puts("  UID   NAME             HOME               FLAGS         DESCRIPTION\n");
    user_info_t u;
    for (uint32_t i = 0; sys_user_get(i, &u) == 0; i++) {
        screen_puts((ci.depth > 0 && (int)u.uid == ci.uid) ? "* " : "  ");
        cu_num_pad(u.uid, 4); screen_puts("  "); cu_pad(u.name, 15); screen_puts("  "); cu_pad(u.home, 17); screen_puts("  ");
        char f[16]; size_t k = 0;
        if (u.flags & USER_F_SUDO) { memcpy(f + k, "sudo", 4); k += 4; }
        if (u.flags & USER_F_LOCKED) { if (k) f[k++] = ','; memcpy(f + k, "locked", 6); k += 6; }
        if (!k) f[k++] = '-';
        f[k] = '\0';
        cu_pad(f, 12); screen_puts("  "); screen_puts(u.comment); screen_putc('\n');
    }
    screen_puts("(* = you; passwords are stored only as salted SHA-256 hashes)\n");
    return 0;
}

static int cmd_userinfo(int argc, const char **argv) {
    if (argc != 2) return cu_usage("userinfo <user>");
    user_info_t u; int rc = sys_user_lookup(argv[1], 0, &u);
    if (rc != 0) return user_error("userinfo", argv[1], rc);
    screen_puts("User "); screen_puts(u.name); screen_putc('\n');
    screen_puts("  UID / GID     : "); screen_put_dec(u.uid); screen_puts(" / "); screen_put_dec(u.gid); screen_putc('\n');
    screen_puts("  Home          : "); screen_puts(u.home); screen_putc('\n');
    screen_puts("  Description   : "); screen_puts(u.comment); screen_putc('\n');
    screen_puts("  Flags         : "); put_flags(u.flags); screen_putc('\n');
    screen_puts("  Logins        : "); screen_put_dec(u.logins);
    if (u.last_login) { screen_puts(" (last at "); put_time(u.last_login); screen_puts(" after boot)"); }
    screen_putc('\n');
    screen_puts("  Failed logins : "); screen_put_dec(u.failures); screen_puts(" since the last success\n");
    return 0;
}

static int cmd_homedir(int argc, const char **argv) {
    if (argc > 2) return cu_usage("homedir [user]");
    user_info_t u; int rc;
    if (argc == 2) rc = sys_user_lookup(argv[1], 0, &u);
    else { int uid = sys_getuid(); rc = uid < 0 ? uid : sys_user_lookup(NULL, uid, &u); }
    if (rc != 0) return user_error("homedir", argc == 2 ? argv[1] : NULL, rc);
    screen_puts(u.home); screen_putc('\n');
    return 0;
}

/* ---------- login / logout / su / sudo ---------- */
static int cmd_login(int argc, const char **argv) {
    if (argc != 2) return cu_usage("login <user>");
    char pw[PW_BUF];
    if (!cu_read_secret("Password: ", pw, sizeof pw)) { cu_wipe(pw, sizeof pw); return cu_error("login", "cancelled."); }
    int rc = sys_login(argv[1], pw);
    cu_wipe(pw, sizeof pw);
    if (rc == SYS_EAUTH) { cu_auth_delay(); err_begin("login"); screen_puts("login incorrect.\n"); return 1; }   /* same message for a wrong name or password */
    if (rc != 0) return user_error("login", argv[1], rc);
    user_info_t u;
    if (sys_user_lookup(argv[1], 0, &u) == 0) sys_chdir(u.home);
    screen_puts("Logged in as "); screen_puts(argv[1]); screen_putc('\n');
    return 0;
}

static int cmd_logout(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("logout");
    cred_info_t before; if (!get_cred("logout", &before)) return 1;
    int rc = sys_logout();
    if (rc != 0) return user_error("logout", NULL, rc);
    cred_info_t after; sys_getcred(&after);
    if (after.depth > 0) { screen_puts("Back to "); screen_puts(after.name); screen_putc('\n'); }
    else screen_puts("Logged out.\n");        /* the shell now shows the login prompt */
    return 0;
}

static int cmd_su(int argc, const char **argv) {
    if (argc > 2) return cu_usage("su [user]     (default: root; 'logout' returns to your own session)");
    const char *target = argc == 2 ? argv[1] : "root";
    cred_info_t ci; if (!get_cred("su", &ci)) return 1;
    char pw[PW_BUF]; pw[0] = '\0';
    if (!(ci.uid == (int)UID_ROOT && !ci.sudo) && !cu_read_secret("Password: ", pw, sizeof pw)) { cu_wipe(pw, sizeof pw); return cu_error("su", "cancelled."); }
    int rc = sys_su(target, pw);
    cu_wipe(pw, sizeof pw);
    if (rc == SYS_EAUTH) { cu_auth_delay(); return user_error("su", target, rc); }
    if (rc != 0) return user_error("su", target, rc);
    screen_puts("Now acting as "); screen_puts(target); screen_puts(". Type 'logout' to go back.\n");
    return 0;
}

static int cmd_sudo(int argc, const char **argv) {
    if (argc < 2) return cu_usage("sudo <command> [args...]     (runs ONE command as root; asks for your own password)");
    static const char *const refused[] = { "sudo", "login", "logout", "su" };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++)
        if (!strcmp(argv[1], refused[i])) return cu_error("sudo", "that command changes the session and cannot be run through sudo.");
    const command_t *c = command_find(argv[1]);
    if (!c) { err_begin("sudo"); screen_puts("unknown command '"); screen_puts(argv[1]); screen_puts("'.\n"); return 127; }
    cred_info_t ci; if (!get_cred("sudo", &ci)) return 1;
    if (ci.depth == 0) return user_error("sudo", NULL, SYS_ENOUSER);
    bool elevate = ci.euid != (int)UID_ROOT;
    if (elevate) {
        char pw[PW_BUF], prompt[40]; size_t n = 0;
        const char *head = "[sudo] password for ";
        for (size_t i = 0; head[i]; i++) prompt[n++] = head[i];
        for (size_t i = 0; ci.name[i] && n < sizeof prompt - 3; i++) prompt[n++] = ci.name[i];
        prompt[n++] = ':'; prompt[n++] = ' '; prompt[n] = '\0';
        if (!cu_read_secret(prompt, pw, sizeof pw)) { cu_wipe(pw, sizeof pw); return cu_error("sudo", "cancelled."); }
        int rc = sys_sudo_begin(pw);
        cu_wipe(pw, sizeof pw);
        if (rc == SYS_EAUTH) { cu_auth_delay(); err_begin("sudo"); screen_puts("incorrect password.\n"); return 1; }
        if (rc == SYS_EACCES) { err_begin("sudo"); screen_puts(ci.name); screen_puts(" is not allowed to use sudo. This incident is recorded (see 'audit').\n"); return 1; }
        if (rc != 0) return user_error("sudo", NULL, rc);
    }
    int status = c->handler ? c->handler(argc - 1, argv + 1) : command_stub(c, argc - 1, argv + 1);
    if (elevate) sys_sudo_end();               /* always drop back, whatever the command returned */
    return status;
}

/* ---------- account administration ---------- */
static int cmd_useradd(int argc, const char **argv) {
    uint32_t flags = 0; const char *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s")) flags |= USER_F_SUDO;
        else if (!name && argv[i][0] != '-') name = argv[i];
        else return cu_usage("useradd [-s] <name>     (-s: the new user may use sudo)");
    }
    if (!name) return cu_usage("useradd [-s] <name>     (-s: the new user may use sudo)");
    cred_info_t ci; if (!get_cred("useradd", &ci)) return 1;
    if (ci.euid != (int)UID_ROOT) return user_error("useradd", name, SYS_EACCES);
    char pw[PW_BUF];
    if (!read_new_password("useradd", pw, "New password: ")) { cu_wipe(pw, sizeof pw); return 1; }
    int uid = sys_user_add(name, pw, flags);
    cu_wipe(pw, sizeof pw);
    if (uid < 0) return user_error("useradd", name, uid);
    screen_puts("Created user '"); screen_puts(name); screen_puts("' (uid "); screen_put_dec((uint32_t)uid); screen_puts("), home /home/"); screen_puts(name); screen_putc('\n');
    return 0;
}

static int cmd_userdel(int argc, const char **argv) {
    bool rm = argc == 3 && !strcmp(argv[1], "-r");
    if (!(argc == 2 || rm)) return cu_usage("userdel [-r] <user>     (-r: also delete the home directory)");
    const char *name = argv[argc - 1];
    int rc = sys_user_del(name, rm);
    if (rc != 0) return user_error("userdel", name, rc);
    screen_puts("Deleted user '"); screen_puts(name); screen_puts(rm ? "' and the home directory\n" : "' (files owned by the old uid are kept)\n");
    return 0;
}

static int cmd_passwd(int argc, const char **argv) {
    if (argc > 2) return cu_usage("passwd [user]");
    cred_info_t ci; if (!get_cred("passwd", &ci)) return 1;
    if (ci.depth == 0) return user_error("passwd", NULL, SYS_ENOUSER);
    const char *target = argc == 2 ? argv[1] : ci.name;
    char oldpw[PW_BUF], newpw[PW_BUF];
    bool need_old = ci.euid != (int)UID_ROOT;
    if (need_old && !cu_read_secret("Current password: ", oldpw, sizeof oldpw)) { cu_wipe(oldpw, sizeof oldpw); return cu_error("passwd", "cancelled."); }
    if (!read_new_password("passwd", newpw, "New password: ")) { cu_wipe(oldpw, sizeof oldpw); cu_wipe(newpw, sizeof newpw); return 1; }
    int rc = sys_user_passwd(target, need_old ? oldpw : NULL, newpw);
    cu_wipe(oldpw, sizeof oldpw); cu_wipe(newpw, sizeof newpw);
    if (rc == SYS_EAUTH) { cu_auth_delay(); err_begin("passwd"); screen_puts("current password is wrong; nothing was changed.\n"); return 1; }
    if (rc != 0) return user_error("passwd", target, rc);
    screen_puts("Password updated for "); screen_puts(target); screen_putc('\n');
    return 0;
}

static int do_mod(const char *cmd, const char *name, int op, const char *value, const char *done) {
    int rc = sys_user_mod(name, op, value);
    if (rc != 0) return user_error(cmd, name, rc);
    screen_puts(name); screen_putc(' '); screen_puts(done); screen_putc('\n');
    return 0;
}
static int cmd_lockuser(int argc, const char **argv)   { return argc == 2 ? do_mod("lockuser", argv[1], USER_MOD_LOCK, NULL, "is now locked (cannot log in)") : cu_usage("lockuser <user>"); }
static int cmd_unlockuser(int argc, const char **argv) { return argc == 2 ? do_mod("unlockuser", argv[1], USER_MOD_UNLOCK, NULL, "is unlocked (failed-login counter reset)") : cu_usage("unlockuser <user>"); }

static int cmd_usermod(int argc, const char **argv) {
    const char *u = "usermod <user> lock | unlock | sudo on | sudo off | comment <text>";
    if (argc < 3) return cu_usage(u);
    if (argc == 3 && !strcmp(argv[2], "lock"))   return cmd_lockuser(2, argv);
    if (argc == 3 && !strcmp(argv[2], "unlock")) return cmd_unlockuser(2, argv);
    if (argc == 4 && !strcmp(argv[2], "sudo") && !strcmp(argv[3], "on"))  return do_mod("usermod", argv[1], USER_MOD_SUDO_ON, NULL, "may now use sudo");
    if (argc == 4 && !strcmp(argv[2], "sudo") && !strcmp(argv[3], "off")) return do_mod("usermod", argv[1], USER_MOD_SUDO_OFF, NULL, "may no longer use sudo");
    if (argc == 4 && !strcmp(argv[2], "comment")) return do_mod("usermod", argv[1], USER_MOD_COMMENT, argv[3], "description updated");
    return cu_usage(u);
}

/* ---------- sessions and the audit trail ---------- */
static int cmd_who(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("who");
    cred_info_t ci; if (!get_cred("who", &ci)) return 1;
    if (ci.depth == 0) { screen_puts("(nobody is logged in)\n"); return 0; }
    cu_pad(ci.name, 10); screen_puts("console   since "); put_time(ci.login_tick); screen_puts(" after boot");
    if (ci.depth > 1) { screen_puts("   (su depth "); screen_put_dec((uint32_t)ci.depth); screen_putc(')'); }
    screen_putc('\n');
    return 0;
}

static int cmd_sessions(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("sessions");
    cred_info_t ci; if (!get_cred("sessions", &ci)) return 1;
    screen_puts("Console session (MiniOS has one console and one identity at a time)\n");
    screen_puts("  Logged in as    : "); screen_puts(ci.depth ? ci.name : "(nobody)"); screen_putc('\n');
    screen_puts("  Acting as       : "); screen_puts(ci.ename); screen_puts(ci.sudo ? "  (sudo active)\n" : "\n");
    screen_puts("  Nested su depth : "); screen_put_dec((uint32_t)ci.depth); screen_puts(" of "); screen_put_dec(SESSION_MAX); screen_putc('\n');
    screen_puts("  Umask           : 0"); put_octal3((uint32_t)ci.umask); screen_putc('\n');
    screen_puts("  Session started : "); put_time(ci.login_tick); screen_puts(" after boot\n");
    return 0;
}

static void print_event(const audit_event_t *e) {
    char who[20];
    cu_num_pad(e->tick / TIMER_HZ, 6); screen_puts(" s  "); cu_pad(audit_event_name(e->type), 14);
    if (e->uid == (int32_t)UID_NOBODY) screen_puts("(nobody)   "); else { cu_uid_name((uint32_t)e->uid, who, sizeof who); cu_pad(who, 10); screen_putc(' '); }
    cu_pad(e->subject, 10); screen_putc(' ');
    if (e->result == 0) screen_puts("ok"); else screen_puts(sys_errname(e->result));
    screen_putc('\n');
}

static int show_events(const char *cmd, int argc, const char **argv, bool logins_only) {
    uint32_t limit = 10;
    if (argc > 2 || (argc == 2 && !cu_parse_count(argv[1], &limit, 100))) { char u[32]; size_t n = strlen(cmd); memcpy(u, cmd, n); memcpy(u + n, " [count]", 9); return cu_usage(u); }
    int total = sys_audit_get(0, NULL);
    if (total < 0) return cu_error(cmd, sys_strerror(total));
    audit_event_t shown[AUDIT_MAX]; uint32_t n = 0;
    for (int i = 0; i < total; i++) {
        audit_event_t e;
        if (sys_audit_get((uint32_t)i, &e) != 0) continue;
        bool is_login = e.type == AUDIT_LOGIN || e.type == AUDIT_LOGIN_FAIL || e.type == AUDIT_LOGOUT || e.type == AUDIT_SU || e.type == AUDIT_SU_FAIL;
        if (!logins_only || is_login) shown[n++] = e;
    }
    uint32_t from = n > limit ? n - limit : 0;
    if (n == 0) { screen_puts("(no events recorded yet)\n"); return 0; }
    screen_puts("  TIME     EVENT          BY         SUBJECT    RESULT\n");
    for (uint32_t i = from; i < n; i++) print_event(&shown[i]);
    screen_puts("(the last "); screen_put_dec(AUDIT_MAX); screen_puts(" events are kept in memory; they are lost at reboot)\n");
    return 0;
}
static int cmd_last(int argc, const char **argv)  { return show_events("last", argc, argv, true); }
static int cmd_audit(int argc, const char **argv) { return show_events("audit", argc, argv, false); }

static int cmd_motd(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("motd");
    char *text; size_t len;
    if (!cu_load_file("motd", "/etc/motd", &text, &len)) return 1;
    cu_print_text(text, len); cu_free_file(text);
    return 0;
}

/* ---------- ownership, modes, umask ---------- */
/* "student", "2", or (for chown) "student:admin" pieces. */
static int resolve_id(const char *s, uint32_t *id) {
    uint32_t v;
    if (parse_u32(s, &v) && v <= 0xFFFF) { user_info_t u; int rc = sys_user_lookup(NULL, (int32_t)v, &u); if (rc == 0) *id = v; return rc; }
    user_info_t u; int rc = sys_user_lookup(s, 0, &u);
    if (rc == 0) *id = u.uid;
    return rc;
}

static int apply_owner(const char *cmd, bool has_uid, uint32_t uid, bool has_gid, uint32_t gid, int argc, const char **argv, int first) {
    int status = 0;
    for (int i = first; i < argc; i++) {
        int rc = sys_chown(argv[i], has_uid ? uid : UID_KEEP, has_gid ? gid : UID_KEEP);
        if (rc == SYS_EACCES) { err_begin(cmd); screen_puts("permission denied: only root may change ownership (use 'sudo "); screen_puts(cmd); screen_puts(" ...').\n"); status = 1; }
        else if (rc != 0) status = cu_fs_error(cmd, argv[i], rc, CU_ANY);
    }
    return status;
}

static int cmd_chown(int argc, const char **argv) {
    if (argc < 3) return cu_usage("chown <user[:group]> <path...>");
    char spec[40]; size_t n = strlen(argv[1]);
    if (n >= sizeof spec) return cu_error("chown", "owner name too long.");
    memcpy(spec, argv[1], n + 1);
    char *colon = NULL; for (size_t i = 0; spec[i]; i++) if (spec[i] == ':') { colon = &spec[i]; break; }
    const char *gname = NULL;
    if (colon) { *colon = '\0'; gname = colon + 1; }
    uint32_t uid = 0, gid = 0; bool has_uid = spec[0] != '\0', has_gid = gname && gname[0];
    if (!has_uid && !has_gid) return cu_usage("chown <user[:group]> <path...>");
    int rc;
    if (has_uid && (rc = resolve_id(spec, &uid)) != 0) return user_error("chown", spec, rc);
    if (has_gid && (rc = resolve_id(gname, &gid)) != 0) return user_error("chown", gname, rc);
    return apply_owner("chown", has_uid, uid, has_gid, gid, argc, argv, 2);
}

static int cmd_chgrp(int argc, const char **argv) {
    if (argc < 3) return cu_usage("chgrp <group> <path...>     (groups are the per-user groups: use a user name)");
    uint32_t gid; int rc = resolve_id(argv[1], &gid);
    if (rc != 0) return user_error("chgrp", argv[1], rc);
    return apply_owner("chgrp", false, 0, true, gid, argc, argv, 2);
}

static int cmd_umask(int argc, const char **argv) {
    if (argc > 2) return cu_usage("umask [octal-mask]     e.g. umask 077");
    if (argc == 2) {
        uint32_t v = 0; size_t n = strlen(argv[1]);
        if (n == 0 || n > 4) return cu_error("umask", "use an octal mask such as 022 or 077.");
        for (size_t i = 0; i < n; i++) { if (argv[1][i] < '0' || argv[1][i] > '7') return cu_error("umask", "use an octal mask such as 022 or 077."); v = v * 8 + (uint32_t)(argv[1][i] - '0'); }
        int rc = sys_umask((int32_t)v);
        if (rc < 0) return cu_error("umask", sys_strerror(rc));
        screen_puts("umask is now 0"); put_octal3(v); screen_puts("  (new files get mode "); put_octal3(0666 & ~v); screen_puts(", new directories "); put_octal3(0777 & ~v); screen_puts(")\n");
        return 0;
    }
    int m = sys_umask(-1);
    screen_putc('0'); put_octal3((uint32_t)m); screen_putc('\n');
    return 0;
}

static int cmd_whoowns(int argc, const char **argv) {
    if (argc < 2) return cu_usage("whoowns <path...>");
    int status = 0;
    for (int i = 1; i < argc; i++) {
        fs_stat_t st; int rc = sys_stat(argv[i], &st);
        if (rc != 0) { status = cu_fs_error("whoowns", argv[i], rc, CU_ANY); continue; }
        char u[20], g[20]; cu_uid_name(st.uid, u, sizeof u); cu_uid_name(st.gid, g, sizeof g);
        screen_puts(argv[i]); screen_puts(": "); screen_puts(u); screen_putc(':'); screen_puts(g); screen_putc('\n');
    }
    return status;
}

static int cmd_perms(int argc, const char **argv) {
    if (argc < 2) return cu_usage("perms <path...>");
    int status = 0;
    for (int i = 1; i < argc; i++) {
        fs_stat_t st; int rc = sys_stat(argv[i], &st);
        if (rc != 0) { status = cu_fs_error("perms", argv[i], rc, CU_ANY); continue; }
        char m[11], u[20], g[20]; fs_mode_string(st.type, st.mode, m); cu_uid_name(st.uid, u, sizeof u); cu_uid_name(st.gid, g, sizeof g);
        screen_puts(m); screen_puts("  0"); put_octal3(st.mode); screen_puts("  "); screen_puts(u); screen_putc(':'); screen_puts(g); screen_putc(' '); screen_puts(argv[i]); screen_putc('\n');
    }
    return status;
}

/* access <user> <path>: which of r/w/x that user gets from the file's own mode bits (same rule MiniFS applies). */
static int cmd_access(int argc, const char **argv) {
    if (argc != 3) return cu_usage("access <user> <path>     (checks the file's own mode bits, not the directories above it)");
    user_info_t u; int rc = sys_user_lookup(argv[1], 0, &u);
    if (rc != 0) return user_error("access", argv[1], rc);
    fs_stat_t st; rc = sys_stat(argv[2], &st);
    if (rc != 0) return cu_fs_error("access", argv[2], rc, CU_ANY);
    screen_puts(u.name); screen_puts(" on "); screen_puts(argv[2]); screen_puts(":");
    static const struct { int bit; const char *name; } want[] = { { FS_R, "read" }, { FS_W, "write" }, { FS_X, "execute" } };
    for (size_t i = 0; i < 3; i++) {
        screen_putc(' '); screen_puts(want[i].name); screen_putc('=');
        screen_puts(fs_mode_allows(st.mode, st.uid, st.gid, u.uid, u.gid, want[i].bit) ? "yes" : "no");
    }
    screen_putc('\n');
    return 0;
}

/* ---------- security summary and password tools ---------- */
static int cmd_secstatus(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("secstatus");
    cred_info_t ci; if (!get_cred("secstatus", &ci)) return 1;
    uint32_t accounts = 0, locked = 0, sudoers = 0, failures = 0; user_info_t u;
    for (uint32_t i = 0; sys_user_get(i, &u) == 0; i++) { accounts++; if (u.flags & USER_F_LOCKED) locked++; if (u.flags & USER_F_SUDO) sudoers++; failures += u.failures; }
    int events = sys_audit_get(0, NULL);
    screen_puts("Security summary\n");
    screen_puts("  Accounts          : "); screen_put_dec(accounts); screen_puts(" ("); screen_put_dec(locked); screen_puts(" locked, "); screen_put_dec(sudoers); screen_puts(" may use sudo)\n");
    screen_puts("  Password storage  : salted SHA-256, "); screen_put_dec(USER_PASS_ROUNDS); screen_puts(" rounds, "); screen_put_dec(USER_SALT_LEN); screen_puts("-byte salt (no plaintext kept)\n");
    screen_puts("  Failed logins now : "); screen_put_dec(failures); screen_puts(" (counted since each account's last success)\n");
    screen_puts("  Current user      : "); screen_puts(ci.depth ? ci.name : "(nobody)"); screen_puts(ci.sudo ? " (sudo active)\n" : "\n");
    screen_puts("  Umask             : 0"); put_octal3((uint32_t)ci.umask); screen_putc('\n');
    screen_puts("  Audit events kept : "); screen_put_dec(events > 0 ? (uint32_t)events : 0); screen_putc('\n');
    screen_puts("  NOT provided      : ring 3 isolation, per-process credentials, persistent accounts\n");
    return 0;
}

static void put_hex(const uint8_t *b, size_t n) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { screen_putc(d[b[i] >> 4]); screen_putc(d[b[i] & 15]); }
}

static int cmd_hashpw(int argc, const char **argv) {
    (void)argv;
    if (argc != 1) return cu_usage("hashpw     (asks for the text; shows the salted hash MiniOS would store)");
    char text[PW_BUF];
    if (!cu_read_secret("Text to hash: ", text, sizeof text)) { cu_wipe(text, sizeof text); return cu_error("hashpw", "cancelled or too long."); }
    uint32_t t = sys_time(); uint8_t salt[SHA256_DIGEST], hash[SHA256_DIGEST];
    sha256(&t, sizeof t, salt);                                  /* demonstration salt; real accounts use users_make_salt() */
    users_hash_password(salt, text, hash);
    cu_wipe(text, sizeof text);
    screen_puts("salt : "); put_hex(salt, USER_SALT_LEN); screen_puts("\nhash : "); put_hex(hash, SHA256_DIGEST); screen_putc('\n');
    screen_puts("(the same text with another salt gives a completely different hash)\n");
    return 0;
}

static int cmd_checkpw(int argc, const char **argv) {
    if (argc != 2) return cu_usage("checkpw <user>     (asks for a password and says whether it is right; does not log in)");
    char pw[PW_BUF];
    if (!cu_read_secret("Password: ", pw, sizeof pw)) { cu_wipe(pw, sizeof pw); return cu_error("checkpw", "cancelled."); }
    int rc = sys_verify(argv[1], pw);
    cu_wipe(pw, sizeof pw);
    if (rc == SYS_ENOUSER) return user_error("checkpw", argv[1], rc);
    if (rc != 0) { cu_auth_delay(); screen_puts("incorrect\n"); return 1; }
    screen_puts("correct\n");
    return 0;
}

static const struct { const char *name; cmd_handler_t fn; } bindings[] = {
    { "whoami", cmd_whoami }, { "users", cmd_users }, { "login", cmd_login }, { "logout", cmd_logout }, { "su", cmd_su },
    { "sudo", cmd_sudo }, { "useradd", cmd_useradd }, { "userdel", cmd_userdel }, { "passwd", cmd_passwd }, { "id", cmd_id },
    { "groups", cmd_groups }, { "who", cmd_who }, { "last", cmd_last }, { "usermod", cmd_usermod }, { "userinfo", cmd_userinfo },
    { "lockuser", cmd_lockuser }, { "unlockuser", cmd_unlockuser }, { "homedir", cmd_homedir }, { "sessions", cmd_sessions },
    { "motd", cmd_motd }, { "chown", cmd_chown }, { "chgrp", cmd_chgrp }, { "umask", cmd_umask }, { "access", cmd_access },
    { "perms", cmd_perms }, { "audit", cmd_audit }, { "secstatus", cmd_secstatus }, { "hashpw", cmd_hashpw },
    { "checkpw", cmd_checkpw }, { "whoowns", cmd_whoowns },
};

int user_commands_register(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (command_bind(bindings[i].name, bindings[i].fn) != CMD_BIND_OK) failed++;
    return failed;
}
