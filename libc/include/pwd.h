#pragma once
#include <stdio.h>
#include <sys/types.h>

/* The accounts of /etc/passwd (docs/design/users.md), one per line:
 * name:password:uid:gid:gecos:home:shell. The password field holds "x"
 * when the hash is in /etc/shadow. The functions return pointers to
 * storage that the next call of any of them overwrites. */
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
/* Walk /etc/passwd from the start (setpwent) to the end (NULL). */
struct passwd *getpwent(void);
void setpwent(void);
void endpwent(void);
/* The next valid entry of stream, skipping malformed lines. */
struct passwd *fgetpwent(FILE *stream);
/* The reentrant lookups fill pw with strings kept in buf (size bytes) and
 * store pw or, when there is no such account, NULL in *result. They
 * return 0 or an errno value, ERANGE for a buffer that is too small. */
int getpwnam_r(const char *name, struct passwd *pw, char *buf, size_t size, struct passwd **result);
int getpwuid_r(uid_t uid, struct passwd *pw, char *buf, size_t size, struct passwd **result);
