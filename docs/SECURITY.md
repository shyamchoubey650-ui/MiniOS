# Users, login and permissions (Phase 10)

## Model
- **Accounts** live in a kernel table (`kernel/security/users.c`): uid, gid (= uid, one private group per user), flags
  (`sudo`, `locked`), home directory, failed-login counter. Up to 8 accounts; uids of new users start at 1000 and are never reused.
- **Session**: a stack of up to 4 identities. `login` replaces it, `su` pushes, `logout` pops; an empty stack means
  "logged out" and the shell shows the login prompt. `sudo` does not push: it temporarily makes the *effective* uid 0.
- **Credentials reach MiniFS in one place** (`apply_credentials()` -> `fs_set_user`). MiniFS then applies the owner/group/other
  rwx rule (`fs_mode_allows`, also used by the `access` command) on every operation. uid 0 bypasses the bits.

## Passwords
`hash = SHA-256` applied 1000 times over `previous || salt || password` (8-byte salt per account). Comparison is constant-time.
A wrong user name costs the same hashing time as a wrong password and gives the same error (EAUTH). A locked account reports
ELOCKED only after the correct password. Passwords are read without echo (stars), copied into kernel buffers, wiped after use,
never put in `history`, and never written to the audit log.

## Who may do what
| Action | Allowed for |
|---|---|
| useradd, userdel, usermod, lockuser, chown, chgrp, mkfs | effective uid 0 (root, or `sudo`) |
| passwd (own) | the user, with the current password |
| passwd (other) | root / sudo, no old password needed |
| sudo | accounts with the `sudo` flag (admin), re-entering their own password |
| su | anyone with the target's password; a real root session needs none |
| userdel | not root, not an account that is logged in |

## System calls added (numbers 37-53)
getuid, getcred, chown, umask, login, logout, su, sudo_begin, sudo_end, verify, user_get, user_lookup, user_add, user_del,
user_passwd, user_mod, audit_get. New errors: EAUTH (-27), ELOCKED (-28), ENOUSER (-29).

## Audit events
login, login-failed, logout, su, su-failed, sudo, sudo-failed, user-added, user-deleted, passwd, passwd-failed, user-modified,
denied. Ring of 32, shown by `audit [n]`; `last [n]` shows the login-related subset.

## Tests
`tests/test_sha256.c` (FIPS vectors, padding boundaries, streaming) and `tests/test_users.c` (332 checks: every syscall, every
command, permission failures, lockout, table full, deleted-uid safety, umask, audit ring wrap, leak check).

## Logging and locks (Phase 12)
- Every audit event is also written to the kernel log as source `sec`. `sec` and `shell` entries are PRIVATE: only root can read them
  (`logs`, `dmesg`, `logtail`, `logfind`, the boot copy and the statistics all obey this).
- Passwords are never logged. The shell logs command NAMES only. Control characters in log text become `?`.
- Changing the log (level, clear, trace, schedtrace) needs root, and each change is itself logged with the uid that made it.
- `lock` is enforced by MiniFS against other users; the lock owner and root bypass it (that is why it is called advisory). `protect` blocks everybody.
- Loopback messages are not private; receiving on ports below 1024 needs root, as on Unix.
