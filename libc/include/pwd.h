#pragma once
#include <sys/types.h>

/* The single user of minios: name "user", uid 0, gid 0, home /home, shell
 * /bin/sh. Lookups of any other uid or name fail with ENOENT. */
struct passwd {
    char *pw_name;
    char *pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_gecos;
    char *pw_dir;
    char *pw_shell;
};

struct passwd *getpwuid(uid_t uid);
struct passwd *getpwnam(const char *name);
