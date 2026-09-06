#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <string.h>

static char user_name[] = "user";
static char empty[] = "";
static char home[] = "/home";
static char shell[] = "/bin/sh";
static struct passwd user = { user_name, empty, 0, 0, empty, home, shell };
static char *members[] = { user_name, NULL };
static struct group group = { user_name, empty, 0, members };

struct passwd *getpwuid(uid_t uid)
{
    if (uid != 0) {
        errno = ENOENT;
        return NULL;
    }
    return &user;
}

struct passwd *getpwnam(const char *name)
{
    if (strcmp(name, user_name) != 0) {
        errno = ENOENT;
        return NULL;
    }
    return &user;
}

struct group *getgrgid(gid_t gid)
{
    if (gid != 0) {
        errno = ENOENT;
        return NULL;
    }
    return &group;
}

struct group *getgrnam(const char *name)
{
    if (strcmp(name, user_name) != 0) {
        errno = ENOENT;
        return NULL;
    }
    return &group;
}
