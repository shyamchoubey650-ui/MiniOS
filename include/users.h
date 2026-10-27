#ifndef USERS_H
#define USERS_H
/* MiniOS user manager (Phase 10): accounts, salted password hashes, the login session,
 * su / sudo, the umask and a security audit trail.
 *
 * REAL:      account table, SHA-256 salted+iterated password hashes (no plaintext is stored),
 *            login / logout / su (stacked sessions) / sudo (temporary root), account locking,
 *            file ownership through MiniFS credentials, per-session umask, audit ring buffer.
 * LIMITS:    ONE console session at a time and ONE set of credentials for the whole kernel:
 *            background tasks run with whatever identity is active (there are no per-process
 *            credentials yet, and everything still runs in ring 0). Salts come from the timer
 *            and a counter, not a hardware random source. The table is RAM only (volatile).
 * All functions return 0 / a value >= 0 on success or a negative SYS_E* code (include/syscall.h). */
#include "include/types.h"

#define USER_NAME_MAX     15
#define USER_COMMENT_MAX  31
#define USER_HOME_MAX     31
#define USER_MAX          8        /* accounts in the table */
#define USER_PASS_MIN     4
#define USER_PASS_MAX     32
#define SESSION_MAX       4        /* nested su sessions */
#define USER_PASS_ROUNDS  1000     /* SHA-256 iterations per password hash */
#define USER_SALT_LEN     8
#define AUDIT_MAX         32       /* events kept in the ring buffer */

#define UID_ROOT      0
#define UID_NOBODY    65534        /* identity while nobody is logged in */
#define UID_KEEP      0xFFFFFFFFu  /* chown/chgrp: "leave this field unchanged" */

#define USER_F_SUDO    1u          /* may run commands as root with sudo */
#define USER_F_LOCKED  2u          /* cannot log in */

/* users_modify() operations */
#define USER_MOD_LOCK     1
#define USER_MOD_UNLOCK   2
#define USER_MOD_SUDO_ON  3
#define USER_MOD_SUDO_OFF 4
#define USER_MOD_COMMENT  5

typedef struct {                   /* what any caller may see: never the hash or salt */
    uint16_t uid, gid;
    uint32_t flags, failures, logins, last_login;   /* last_login = timer ticks, 0 = never */
    char     name[USER_NAME_MAX + 1];
    char     comment[USER_COMMENT_MAX + 1];
    char     home[USER_HOME_MAX + 1];
} user_info_t;

typedef struct {                   /* the caller's credentials */
    int32_t  uid, euid, gid, egid, umask, sudo, depth;   /* depth = nested sessions; 0 = logged out */
    uint32_t login_tick;
    char     name[USER_NAME_MAX + 1], ename[USER_NAME_MAX + 1];
} cred_info_t;

typedef enum {
    AUDIT_LOGIN = 1, AUDIT_LOGIN_FAIL, AUDIT_LOGOUT, AUDIT_SU, AUDIT_SU_FAIL, AUDIT_SUDO, AUDIT_SUDO_FAIL,
    AUDIT_USER_ADD, AUDIT_USER_DEL, AUDIT_PASSWD, AUDIT_PASSWD_FAIL, AUDIT_USER_MOD, AUDIT_DENIED
} audit_type_t;

typedef struct {
    uint32_t tick;
    int32_t  type, uid, result;    /* uid = who acted (UID_NOBODY if nobody); result = 0 or SYS_E* */
    char     subject[USER_NAME_MAX + 1];   /* the account the event is about */
} audit_event_t;

const char *audit_event_name(int type);

void users_init(void);                                  /* built-in accounts, home directories, logged out */
void users_fs_sync(void);                               /* recreate homes and /etc/passwd (after mkfs) */

int  users_login(const char *name, const char *password);        /* replaces the whole session stack */
int  users_logout(void);                                          /* pops one session; 0 sessions = logged out */
int  users_su(const char *name, const char *password);            /* root needs no password */
int  users_sudo_begin(const char *password);                      /* caller's OWN password; needs USER_F_SUDO */
int  users_sudo_end(void);
int  users_verify(const char *name, const char *password);        /* 0 or SYS_EAUTH / SYS_ENOUSER; no session change */

int  users_add(const char *name, const char *password, uint32_t flags, uint16_t *uid_out);   /* root only */
int  users_del(const char *name, bool remove_home);                                          /* root only */
int  users_set_password(const char *name, const char *old_pw, const char *new_pw);           /* own: old_pw needed; root: no */
int  users_modify(const char *name, int op, const char *value);                              /* root only */

int  users_get(uint32_t index, user_info_t *out);                 /* index-th account, 0..; SYS_ENOENT past the end */
int  users_lookup(const char *name, int32_t uid, user_info_t *out);   /* name != NULL: by name, else by uid */
bool users_uid_exists(uint32_t uid);
bool users_gid_exists(uint32_t gid);                              /* groups are the per-user groups: gid == uid */

int  users_getuid(void);                                          /* real uid, or SYS_ENOUSER when logged out */
int  users_euid(void);                                            /* effective uid (0 while sudo is active) */
void users_cred(cred_info_t *out);
int  users_umask(int32_t new_mask);                               /* < 0: just read; returns the previous mask */

int  users_audit_get(uint32_t index, audit_event_t *out);         /* 0 = oldest kept; (NULL) returns the count */
void users_hash_password(const uint8_t salt[8], const char *password, uint8_t out[32]);   /* exposed for hashpw and tests */
void users_make_salt(const char *seed, uint8_t salt[8]);
#endif
