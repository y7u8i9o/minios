#pragma once
#include <sys/types.h>

/* The single group of minios: name "user", gid 0, with the one user as
 * its member. */
struct group {
    char *gr_name;
    char *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};

struct group *getgrgid(gid_t gid);
struct group *getgrnam(const char *name);
