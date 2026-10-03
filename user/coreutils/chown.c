/* chown and chgrp: change the owner and group of files (chown(1)).
 *
 *     chown [-R] [-h] owner[:group] file...
 *     chgrp [-R] [-h] group file...
 *
 * Owner and group are names or numbers. chown with "owner:" sets the group
 * to the login group of owner, and ":group" changes only the group. -h
 * changes a symbolic link itself instead of the file it leads to. chgrp.c
 * builds this file with CHGRP defined. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>

#ifdef CHGRP
#define PROG "chgrp"
#else
#define PROG "chown"
#endif

static int status;

#ifndef CHGRP
/* A number, or the id of a name in the database. Returns -1 when neither. */
static int parse_user(const char *s, uid_t *out, gid_t *login_group)
{
    struct passwd *pw = getpwnam(s);
    if (pw) {
        *out = pw->pw_uid;
        if (login_group)
            *login_group = pw->pw_gid;
        return 0;
    }
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (!*s || *end)
        return -1;
    *out = (uid_t)v;
    if (login_group) {
        pw = getpwuid((uid_t)v);
        *login_group = pw ? pw->pw_gid : (gid_t)-1;
    }
    return 0;
}
#endif

static int parse_group(const char *s, gid_t *out)
{
    struct group *gr = getgrnam(s);
    if (gr) {
        *out = gr->gr_gid;
        return 0;
    }
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (!*s || *end)
        return -1;
    *out = (gid_t)v;
    return 0;
}

static void change(const char *path, uid_t uid, gid_t gid, int recursive, int nofollow)
{
    int r = nofollow ? lchown(path, uid, gid) : chown(path, uid, gid);
    if (r < 0) {
        fprintf(stderr, PROG ": %s: %s\n", path, strerror(errno));
        status = 1;
    }
    struct stat st;
    if (!recursive || lstat(path, &st) < 0 || !S_ISDIR(st.st_mode))
        return;
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, PROG ": %s: %s\n", path, strerror(errno));
        status = 1;
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char sub[1024];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        /* Links met during the walk are changed themselves, not followed. */
        change(sub, uid, gid, 1, 1);
    }
    closedir(d);
}

static int usage(void)
{
#ifdef CHGRP
    fprintf(stderr, "usage: chgrp [-R] [-h] group file...\n");
#else
    fprintf(stderr, "usage: chown [-R] [-h] owner[:group] file...\n");
#endif
    return 2;
}

int main(int argc, char **argv)
{
    int recursive = 0, nofollow = 0, opt;
    while ((opt = getopt(argc, argv, "Rh")) != -1) {
        if (opt == 'R')
            recursive = 1;
        else if (opt == 'h')
            nofollow = 1;
        else
            return usage();
    }
    if (argc - optind < 2)
        return usage();
    char spec[128];
    snprintf(spec, sizeof spec, "%s", argv[optind++]);
    uid_t uid = (uid_t)-1;
    gid_t gid = (gid_t)-1;
#ifdef CHGRP
    if (parse_group(spec, &gid) < 0) {
        fprintf(stderr, "chgrp: %s: no such group\n", spec);
        return 1;
    }
#else
    char *colon = strchr(spec, ':');
    if (colon)
        *colon = '\0';
    gid_t login_group = (gid_t)-1;
    if (spec[0] && parse_user(spec, &uid, &login_group) < 0) {
        fprintf(stderr, "chown: %s: no such user\n", spec);
        return 1;
    }
    if (colon && colon[1] && parse_group(colon + 1, &gid) < 0) {
        fprintf(stderr, "chown: %s: no such group\n", colon + 1);
        return 1;
    }
    if (colon && !colon[1] && spec[0])
        gid = login_group;
    if (!spec[0] && !(colon && colon[1]))
        return usage();
#endif
    for (int i = optind; i < argc; i++)
        change(argv[i], uid, gid, recursive, nofollow);
    return status;
}
