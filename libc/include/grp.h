#pragma once
#include <stdio.h>
#include <sys/types.h>

/* The groups of /etc/group, one per line: name:password:gid:members,
 * with the members separated by commas. The functions return pointers to
 * storage that the next call of any of them overwrites. */
struct group {
    char *gr_name;
    char *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};

struct group *getgrgid(gid_t gid);
struct group *getgrnam(const char *name);
struct group *getgrent(void);
void setgrent(void);
void endgrent(void);
struct group *fgetgrent(FILE *stream);
/* Reentrant lookups in the manner of getpwnam_r. The member list is stored
 * in buf as well. */
int getgrnam_r(const char *name, struct group *gr, char *buf, size_t size, struct group **result);
int getgrgid_r(gid_t gid, struct group *gr, char *buf, size_t size, struct group **result);
/* The groups of user: group first, then every group that lists user as a
 * member. At most *ngroups are stored. Returns the count, or -1 with the
 * count needed in *ngroups when the array is too small. */
int getgrouplist(const char *user, gid_t group, gid_t *groups, int *ngroups);
/* Set the supplementary groups of the caller from getgrouplist. */
int initgroups(const char *user, gid_t group);
