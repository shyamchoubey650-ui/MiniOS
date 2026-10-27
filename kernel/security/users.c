/* User manager: accounts, password hashes, the login session, su/sudo, umask and the audit trail.
 * See include/users.h for what is real and what is limited.
 *
 * Password storage:  hash = SHA-256 iterated PASS_ROUNDS times over (previous hash || salt || password),
 * with an 8-byte salt per account. Only salt and hash are kept; the plaintext never leaves the call
 * that received it and temporary copies are wiped. Comparisons take the same time for any input. */
#include "include/users.h"
#include "include/logger.h"
#include "include/syscall.h"
#include "include/filesystem.h"
#include "include/sha256.h"
#include "include/timer.h"
#include "include/string.h"

#define PASS_ROUNDS   USER_PASS_ROUNDS
#define SALT_LEN      USER_SALT_LEN
#define FIRST_NEW_UID 1000                         /* accounts created with useradd start here */
#define PASSWD_FILE   "/etc/passwd"
#define ROOT_HOME     "/root"
#define DEFAULT_UMASK 022

typedef struct { bool used; user_info_t pub; uint8_t salt[SALT_LEN]; uint8_t hash[SHA256_DIGEST]; } user_rec_t;

static user_rec_t   db[USER_MAX];
static uint32_t     next_uid = FIRST_NEW_UID;
static uint16_t     sess_uid[SESSION_MAX];
static uint32_t     sess_tick[SESSION_MAX];
static int          depth;                         /* active sessions; 0 = logged out */
static bool         sudo_on;
static int32_t      cur_umask = DEFAULT_UMASK;
static audit_event_t ring[AUDIT_MAX];
static uint32_t     ring_total;                    /* events ever recorded */
static uint32_t     salt_counter;

/* Built-in accounts. The hashes were produced with users_hash_password() (tests/test_users.c checks them);
 * the plaintext defaults are listed in README.md, not in the kernel image. */
static const struct { const char *name, *comment; uint16_t uid; uint32_t flags; uint8_t salt[SALT_LEN]; uint8_t hash[SHA256_DIGEST]; } builtin[] = {
    { "root", "System administrator", 0, 0, {0xa3,0xf1,0xc0,0x7b,0x52,0xd9,0xe4,0x86},
      {0xc0,0xbf,0x63,0x5e,0x69,0xcd,0xe5,0x04,0x13,0x83,0xfc,0x80,0x2c,0x95,0x0e,0x9a,0x79,0x8c,0x52,0xd1,0x28,0xd0,0xd1,0x44,0x8f,0xdd,0x03,0x44,0x8f,0xef,0xc4,0x31} },
    { "admin", "Administrator", 1, USER_F_SUDO, {0x5e,0x2b,0x90,0xd4,0xc1,0xa7,0xf3,0x68},
      {0xad,0x49,0x9b,0x76,0x30,0x08,0x8f,0xbb,0xd3,0xb2,0xa7,0x4a,0x84,0x1d,0xad,0x83,0x5b,0xc4,0x53,0xf5,0xd4,0xa2,0xc0,0xba,0x29,0xa9,0x2e,0x81,0x9e,0x9c,0xf0,0x08} },
    { "student", "Student account", 2, 0, {0xc8,0x4d,0x1f,0x6a,0x0b,0x93,0xe2,0x75},
      {0x04,0xde,0x47,0x12,0x66,0x4c,0xad,0x81,0x77,0xfc,0x33,0xb7,0x13,0x1e,0x8a,0xf0,0x43,0xec,0xee,0xfe,0x3a,0x83,0x66,0x00,0x52,0x14,0xa1,0x8a,0xcb,0x7f,0x4e,0x26} },
    { "guest", "Guest account", 3, 0, {0x17,0xb6,0xe8,0x30,0x5d,0xca,0x94,0xf2},
      {0xb7,0xc4,0xca,0xa7,0x40,0xdf,0xcd,0x4a,0x4c,0x2e,0xb5,0x01,0x51,0x43,0xc8,0x32,0x99,0xc6,0x01,0xfe,0x53,0x4f,0xbc,0x1f,0x40,0xf2,0xa3,0x90,0x89,0x8f,0x49,0x07} },
};

/* ---------- small helpers ---------- */
static void copy_text(char *dst, size_t cap, const char *src) {
    size_t i = 0; while (src && src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void wipe(void *p, size_t n) { volatile uint8_t *v = p; while (n--) *v++ = 0; }

static bool ct_equal(const uint8_t *a, const uint8_t *b, size_t n) {   /* time does not depend on where they differ */
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static bool name_valid(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > USER_NAME_MAX) return false;
    if (!(s[0] >= 'a' && s[0] <= 'z')) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return strcmp(s, "nobody") != 0;
}

static bool password_valid(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < USER_PASS_MIN || n > USER_PASS_MAX) return false;
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) return false;
    return true;
}

static user_rec_t *find_name(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < USER_MAX; i++) if (db[i].used && !strcmp(db[i].pub.name, name)) return &db[i];
    return NULL;
}
static user_rec_t *find_uid(uint32_t uid) {
    for (int i = 0; i < USER_MAX; i++) if (db[i].used && db[i].pub.uid == uid) return &db[i];
    return NULL;
}

/* ---------- audit trail ---------- */
const char *audit_event_name(int t) {
    switch (t) {
        case AUDIT_LOGIN: return "login";           case AUDIT_LOGIN_FAIL: return "login-failed";
        case AUDIT_LOGOUT: return "logout";         case AUDIT_SU: return "su";
        case AUDIT_SU_FAIL: return "su-failed";     case AUDIT_SUDO: return "sudo";
        case AUDIT_SUDO_FAIL: return "sudo-failed"; case AUDIT_USER_ADD: return "user-added";
        case AUDIT_USER_DEL: return "user-deleted"; case AUDIT_PASSWD: return "passwd";
        case AUDIT_PASSWD_FAIL: return "passwd-failed"; case AUDIT_USER_MOD: return "user-modified";
        case AUDIT_DENIED: return "denied";         default: return "?";
    }
}

static int32_t actor_uid(void) { return depth > 0 ? (int32_t)sess_uid[depth - 1] : (int32_t)UID_NOBODY; }

static void audit(int type, int result, const char *subject) {
    audit_event_t *e = &ring[ring_total % AUDIT_MAX];
    e->tick = timer_ticks(); e->type = type; e->uid = actor_uid(); e->result = result;
    copy_text(e->subject, sizeof e->subject, subject ? subject : "");
    ring_total++;
    klogf(result != 0 || type == AUDIT_LOGIN_FAIL || type == AUDIT_SU_FAIL || type == AUDIT_SUDO_FAIL || type == AUDIT_PASSWD_FAIL || type == AUDIT_DENIED ? LOG_WARN : LOG_INFO,
          "sec", "%s uid=%d subject=%s result=%d", audit_event_name(type), e->uid, subject && *subject ? subject : "-", result);
}

int users_audit_get(uint32_t index, audit_event_t *out) {
    uint32_t kept = ring_total < AUDIT_MAX ? ring_total : AUDIT_MAX;
    if (!out) return (int)kept;
    if (index >= kept) return SYS_ENOENT;
    uint32_t first = ring_total - kept;                       /* sequence number of the oldest kept event */
    *out = ring[(first + index) % AUDIT_MAX];
    return 0;
}

/* ---------- password hashing ---------- */
void users_hash_password(const uint8_t salt[8], const char *password, uint8_t out[32]) {
    size_t n = strlen(password);
    sha256_ctx_t c;
    sha256_init(&c); sha256_update(&c, salt, SALT_LEN); sha256_update(&c, password, n); sha256_final(&c, out);
    for (int i = 1; i < PASS_ROUNDS; i++) {
        sha256_init(&c); sha256_update(&c, out, SHA256_DIGEST); sha256_update(&c, salt, SALT_LEN);
        sha256_update(&c, password, n); sha256_final(&c, out);
    }
}

/* No hardware random source: mix the tick counter, a counter and the account name. Unique, not secret. */
void users_make_salt(const char *seed, uint8_t salt[8]) {
    uint32_t material[3] = { timer_ticks(), ++salt_counter, (uint32_t)ring_total };
    uint8_t d[SHA256_DIGEST];
    sha256_ctx_t c;
    sha256_init(&c); sha256_update(&c, material, sizeof material); sha256_update(&c, seed, strlen(seed)); sha256_final(&c, d);
    memcpy(salt, d, SALT_LEN);
}

static bool password_matches(const user_rec_t *u, const char *pw) {
    uint8_t h[SHA256_DIGEST];
    users_hash_password(u->salt, pw, h);
    bool ok = ct_equal(h, u->hash, SHA256_DIGEST);
    wipe(h, sizeof h);
    return ok;
}

/* A wrong user name costs the same hashing time as a wrong password, so timing does not reveal accounts. */
static void dummy_hash(const char *pw) {
    static const uint8_t salt[SALT_LEN] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t h[SHA256_DIGEST];
    users_hash_password(salt, pw ? pw : "", h);
    wipe(h, sizeof h);
}

/* ---------- effective credentials ---------- */
int users_getuid(void) { return depth > 0 ? (int)sess_uid[depth - 1] : SYS_ENOUSER; }
int users_euid(void)   { return depth == 0 ? (int)UID_NOBODY : sudo_on ? (int)UID_ROOT : (int)sess_uid[depth - 1]; }
static int users_egid(void) {
    if (depth == 0) return (int)UID_NOBODY;
    if (sudo_on) return 0;
    const user_rec_t *u = find_uid(sess_uid[depth - 1]);
    return u ? u->pub.gid : (int)UID_NOBODY;
}

static void apply_credentials(void) {                          /* the one place MiniFS learns who is acting */
    fs_set_user((uint16_t)users_euid(), (uint16_t)users_egid());
    fs_set_umask((uint16_t)cur_umask);
}

bool users_uid_exists(uint32_t uid) { return uid <= 0xFFFF && find_uid(uid) != NULL; }
bool users_gid_exists(uint32_t gid) { return users_uid_exists(gid); }

void users_cred(cred_info_t *o) {
    memset(o, 0, sizeof *o);
    o->depth = depth; o->sudo = sudo_on; o->umask = cur_umask;
    o->uid = users_getuid(); o->euid = users_euid(); o->gid = depth ? (int32_t)(find_uid(sess_uid[depth - 1]) ? find_uid(sess_uid[depth - 1])->pub.gid : UID_NOBODY) : (int32_t)UID_NOBODY;
    o->egid = users_egid();
    o->login_tick = depth ? sess_tick[depth - 1] : 0;
    const user_rec_t *r = depth ? find_uid(sess_uid[depth - 1]) : NULL;
    copy_text(o->name, sizeof o->name, r ? r->pub.name : "nobody");
    copy_text(o->ename, sizeof o->ename, sudo_on ? "root" : o->name);
}

/* ---------- file-system side effects (always done as root) ---------- */
typedef struct { uint16_t uid, gid; } saved_creds_t;
static saved_creds_t root_begin(void) { saved_creds_t s; fs_get_user(&s.uid, &s.gid); fs_set_user(0, 0); return s; }
static void root_end(saved_creds_t s) { fs_set_user(s.uid, s.gid); }

static void ensure_home(const user_info_t *u) {
    fs_stat_t st;
    if (fs_stat(u->home, &st) != FS_OK) fs_mkdir_parents(u->home);
    fs_chown(u->home, u->uid, u->gid);
    fs_chmod(u->home, u->uid == UID_ROOT ? 0700 : 0750);
}

static void write_passwd(void) {                              /* a readable MIRROR; the kernel table is authoritative */
    char text[USER_MAX * 100]; size_t n = 0;
    for (int i = 0; i < USER_MAX; i++) {
        if (!db[i].used) continue;
        const user_info_t *u = &db[i].pub;
        char num[12];
        const char *parts[] = { u->name, ":x:", NULL, ":", NULL, ":", u->comment, ":", u->home, (u->flags & USER_F_LOCKED) ? ":locked" : ":active", "\n" };
        uint32_t vals[2] = { u->uid, u->gid };
        int vi = 0;
        for (size_t p = 0; p < sizeof parts / sizeof parts[0]; p++) {
            const char *s = parts[p];
            if (!s) {
                uint32_t v = vals[vi++]; size_t k = 0; char t[12];
                do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
                size_t m = 0; while (k) num[m++] = t[--k]; num[m] = '\0'; s = num;
            }
            size_t l = strlen(s);
            if (n + l >= sizeof text) break;
            memcpy(text + n, s, l); n += l;
        }
    }
    fs_write(PASSWD_FILE, text, n, FS_WRITE_TRUNCATE);
    fs_chmod(PASSWD_FILE, 0644);
}

void users_fs_sync(void) {
    saved_creds_t s = root_begin();
    for (int i = 0; i < USER_MAX; i++) if (db[i].used) ensure_home(&db[i].pub);
    write_passwd();
    root_end(s);
}

/* ---------- lifecycle ---------- */
void users_init(void) {
    memset(db, 0, sizeof db); memset(ring, 0, sizeof ring);
    ring_total = 0; salt_counter = 0; depth = 0; sudo_on = false; cur_umask = DEFAULT_UMASK; next_uid = FIRST_NEW_UID;
    for (size_t i = 0; i < sizeof builtin / sizeof builtin[0]; i++) {
        user_rec_t *r = &db[i];
        r->used = true;
        r->pub.uid = r->pub.gid = builtin[i].uid; r->pub.flags = builtin[i].flags;
        copy_text(r->pub.name, sizeof r->pub.name, builtin[i].name);
        copy_text(r->pub.comment, sizeof r->pub.comment, builtin[i].comment);
        copy_text(r->pub.home, sizeof r->pub.home, builtin[i].uid == UID_ROOT ? ROOT_HOME : "/home/");
        if (builtin[i].uid != UID_ROOT) copy_text(r->pub.home + 6, sizeof r->pub.home - 6, builtin[i].name);
        memcpy(r->salt, builtin[i].salt, SALT_LEN); memcpy(r->hash, builtin[i].hash, SHA256_DIGEST);
    }
    users_fs_sync();
    apply_credentials();                                       /* logged out: the "nobody" identity */
}

/* ---------- sessions ---------- */
static void begin_session(int new_depth, uint16_t uid) {
    depth = new_depth; sess_uid[depth - 1] = uid; sess_tick[depth - 1] = timer_ticks();
    sudo_on = false; cur_umask = DEFAULT_UMASK;
    apply_credentials();
}

/* Shared by login/su/verify: checks the password and the lock flag, keeps the failure counters. */
static int authenticate(user_rec_t *u, const char *pw, bool skip_password) {
    if (!skip_password) {
        if (!pw || !password_matches(u, pw)) { u->pub.failures++; return SYS_EAUTH; }
        u->pub.failures = 0;
    }
    return (u->pub.flags & USER_F_LOCKED) ? SYS_ELOCKED : 0;
}

int users_login(const char *name, const char *pw) {
    user_rec_t *u = find_name(name);
    if (!u) { dummy_hash(pw); audit(AUDIT_LOGIN_FAIL, SYS_EAUTH, name); return SYS_EAUTH; }
    int rc = authenticate(u, pw, false);
    if (rc) { audit(AUDIT_LOGIN_FAIL, rc, name); return rc; }
    if (depth > 0) audit(AUDIT_LOGOUT, 0, find_uid(sess_uid[0]) ? find_uid(sess_uid[0])->pub.name : "");
    begin_session(1, u->pub.uid);                              /* login replaces any existing session */
    u->pub.logins++; u->pub.last_login = timer_ticks() ? timer_ticks() : 1;
    audit(AUDIT_LOGIN, 0, name);
    return 0;
}

int users_logout(void) {
    if (depth == 0) return SYS_ENOUSER;
    const user_rec_t *u = find_uid(sess_uid[depth - 1]);
    audit(AUDIT_LOGOUT, 0, u ? u->pub.name : "");
    depth--; sudo_on = false; cur_umask = DEFAULT_UMASK;
    if (depth == 0) { saved_creds_t s = root_begin(); fs_chdir("/"); root_end(s); }
    apply_credentials();
    return 0;
}

int users_su(const char *name, const char *pw) {
    if (depth == 0) return SYS_ENOUSER;
    if (depth >= SESSION_MAX) return SYS_ELIMIT;
    user_rec_t *u = find_name(name);
    if (!u) { dummy_hash(pw); audit(AUDIT_SU_FAIL, SYS_ENOUSER, name); return SYS_ENOUSER; }
    bool is_root = sess_uid[depth - 1] == UID_ROOT && !sudo_on;    /* only a real root session skips the password */
    int rc = authenticate(u, pw, is_root);
    if (rc) { audit(AUDIT_SU_FAIL, rc, name); return rc; }
    begin_session(depth + 1, u->pub.uid);
    audit(AUDIT_SU, 0, name);
    return 0;
}

int users_sudo_begin(const char *pw) {
    if (depth == 0) return SYS_ENOUSER;
    if (sudo_on) return SYS_EBUSY;
    user_rec_t *me = find_uid(sess_uid[depth - 1]);
    if (!me) return SYS_ENOUSER;
    if (!(me->pub.flags & USER_F_SUDO)) { audit(AUDIT_SUDO_FAIL, SYS_EACCES, me->pub.name); return SYS_EACCES; }
    int rc = authenticate(me, pw, false);                      /* re-enter YOUR OWN password */
    if (rc) { audit(AUDIT_SUDO_FAIL, rc, me->pub.name); return rc; }
    sudo_on = true;
    apply_credentials();
    audit(AUDIT_SUDO, 0, me->pub.name);
    return 0;
}

int users_sudo_end(void) {
    if (!sudo_on) return SYS_ESTATE;
    sudo_on = false;
    apply_credentials();
    return 0;
}

int users_verify(const char *name, const char *pw) {
    user_rec_t *u = find_name(name);
    if (!u) { dummy_hash(pw); return SYS_ENOUSER; }
    return password_matches(u, pw ? pw : "") ? (u->pub.failures = 0, 0) : (u->pub.failures++, SYS_EAUTH);
}

/* ---------- account administration ---------- */
static int need_root(const char *subject) {
    if (users_euid() == (int)UID_ROOT) return 0;
    audit(AUDIT_DENIED, SYS_EACCES, subject);
    return SYS_EACCES;
}

int users_add(const char *name, const char *pw, uint32_t flags, uint16_t *uid_out) {
    int rc = need_root(name);
    if (rc) return rc;
    if (!name_valid(name) || !password_valid(pw) || (flags & ~USER_F_SUDO)) return SYS_EINVAL;
    if (find_name(name)) return SYS_EEXIST;
    if (next_uid > 60000) return SYS_ELIMIT;
    user_rec_t *r = NULL;
    for (int i = 0; i < USER_MAX; i++) if (!db[i].used) { r = &db[i]; break; }
    if (!r) return SYS_ELIMIT;
    memset(r, 0, sizeof *r);
    r->used = true;
    r->pub.uid = r->pub.gid = (uint16_t)next_uid++;          /* uids are never reused, so old files cannot be inherited */
    r->pub.flags = flags;
    copy_text(r->pub.name, sizeof r->pub.name, name);
    copy_text(r->pub.comment, sizeof r->pub.comment, "User account");
    copy_text(r->pub.home, sizeof r->pub.home, "/home/");
    copy_text(r->pub.home + 6, sizeof r->pub.home - 6, name);
    users_make_salt(name, r->salt);
    users_hash_password(r->salt, pw, r->hash);
    users_fs_sync();
    audit(AUDIT_USER_ADD, 0, name);
    if (uid_out) *uid_out = r->pub.uid;
    return 0;
}

int users_del(const char *name, bool remove_home) {
    int rc = need_root(name);
    if (rc) return rc;
    user_rec_t *u = find_name(name);
    if (!u) return SYS_ENOUSER;
    if (u->pub.uid == UID_ROOT) return SYS_EPERM;
    for (int i = 0; i < depth; i++) if (sess_uid[i] == u->pub.uid) return SYS_EBUSY;      /* logged in (also under su) */
    if (remove_home) {
        saved_creds_t s = root_begin();
        uint32_t removed;
        int frc = fs_remove_tree(u->pub.home, &removed);
        root_end(s);
        if (frc != FS_OK && frc != FS_ERR_NOT_FOUND) return frc;
    }
    wipe(u, sizeof *u);
    users_fs_sync();
    audit(AUDIT_USER_DEL, 0, name);
    return 0;
}

int users_set_password(const char *name, const char *old_pw, const char *new_pw) {
    user_rec_t *u = find_name(name);
    if (!u) return SYS_ENOUSER;
    if (depth == 0) return SYS_EACCES;
    bool admin = users_euid() == (int)UID_ROOT;
    if (!admin) {
        if (sess_uid[depth - 1] != u->pub.uid) { audit(AUDIT_DENIED, SYS_EACCES, name); return SYS_EACCES; }
        if (!old_pw || !password_matches(u, old_pw)) { u->pub.failures++; audit(AUDIT_PASSWD_FAIL, SYS_EAUTH, name); return SYS_EAUTH; }
    }
    if (!password_valid(new_pw)) return SYS_EINVAL;
    users_make_salt(name, u->salt);
    users_hash_password(u->salt, new_pw, u->hash);
    u->pub.failures = 0;
    audit(AUDIT_PASSWD, 0, name);
    return 0;
}

int users_modify(const char *name, int op, const char *value) {
    int rc = need_root(name);
    if (rc) return rc;
    user_rec_t *u = find_name(name);
    if (!u) return SYS_ENOUSER;
    switch (op) {
        case USER_MOD_LOCK:
            if (u->pub.uid == UID_ROOT) return SYS_EPERM;
            for (int i = 0; i < depth; i++) if (sess_uid[i] == u->pub.uid) return SYS_EBUSY;
            u->pub.flags |= USER_F_LOCKED; break;
        case USER_MOD_UNLOCK:  u->pub.flags &= ~USER_F_LOCKED; u->pub.failures = 0; break;
        case USER_MOD_SUDO_ON: u->pub.flags |= USER_F_SUDO; break;
        case USER_MOD_SUDO_OFF:
            if (u->pub.uid == UID_ROOT) return SYS_EPERM;
            u->pub.flags &= ~USER_F_SUDO; break;
        case USER_MOD_COMMENT: {
            size_t n = value ? strlen(value) : 0;
            if (n == 0 || n > USER_COMMENT_MAX) return SYS_EINVAL;
            for (size_t i = 0; i < n; i++) if (value[i] < 32 || value[i] > 126 || value[i] == ':') return SYS_EINVAL;
            copy_text(u->pub.comment, sizeof u->pub.comment, value); break;
        }
        default: return SYS_EINVAL;
    }
    users_fs_sync();
    audit(AUDIT_USER_MOD, 0, name);
    return 0;
}

/* ---------- queries ---------- */
int users_get(uint32_t index, user_info_t *out) {
    uint32_t seen = 0;
    for (int i = 0; i < USER_MAX; i++) {
        if (!db[i].used) continue;
        if (seen++ == index) { *out = db[i].pub; return 0; }
    }
    return SYS_ENOENT;
}

int users_lookup(const char *name, int32_t uid, user_info_t *out) {
    const user_rec_t *u = name ? find_name(name) : (uid >= 0 ? find_uid((uint32_t)uid) : NULL);
    if (!u) return SYS_ENOUSER;
    *out = u->pub;
    return 0;
}

int users_umask(int32_t m) {
    int32_t old = cur_umask;
    if (m < 0) return old;
    if (m > 0777) return SYS_EINVAL;
    cur_umask = m;
    fs_set_umask((uint16_t)cur_umask);
    return old;
}
