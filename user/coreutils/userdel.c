/* userdel: remove an account (useradd(1)).
 *
 *     userdel [-r] name
 *
 * Only root may remove accounts, and root itself cannot be removed. The
 * account leaves /etc/passwd, /etc/shadow and the member lists of
 * /etc/group, and its own group goes when no other account uses it as its
 * group. -r also removes the home directory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <pwd.h>
#include <grp.h>
#include <limits.h>
#include <sys/stat.h>
#include <minios/account.h>

static int remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d)
            return -1;
        struct dirent *e;
        int r = 0;
        while (r == 0 && (e = readdir(d))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char sub[PATH_MAX];
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            r = remove_tree(sub);
        }
        closedir(d);
        return r == 0 ? rmdir(path) : r;
    }
    return unlink(path);
}

/* The lines of /etc/group to rewrite: name dropped from every member
 * list, and the group with gid removed when it is name's own group. */
static int fix_groups(const char *name, gid_t own)
{
    char names[64][33], lines[64][1024];
    int n = 0, remove_own = -1;
    FILE *f = fopen(ACCOUNT_GROUP, "r");
    if (!f)
        return -1;
    struct group *gr;
    while ((gr = fgetgrent(f)) && n < 64) {
        int member = 0;
        for (char **m = gr->gr_mem; *m; m++)
            member |= strcmp(*m, name) == 0;
        if (gr->gr_gid == own && strcmp(gr->gr_name, name) == 0)
            remove_own = n;
        if (!member && remove_own != n)
            continue;
        snprintf(names[n], sizeof names[n], "%s", gr->gr_name);
        int len = snprintf(lines[n], sizeof lines[n], "%s:%s:%u:", gr->gr_name, gr->gr_passwd, gr->gr_gid);
        int first = 1;
        for (char **m = gr->gr_mem; *m; m++) {
            if (strcmp(*m, name) == 0)
                continue;
            len += snprintf(lines[n] + len, sizeof lines[n] - (size_t)len, "%s%s", first ? "" : ",", *m);
            first = 0;
        }
        n++;
    }
    fclose(f);
    /* The own group remains while another account has it as its group. */
    if (remove_own >= 0) {
        setpwent();
        struct passwd *pw;
        while ((pw = getpwent()))
            if (pw->pw_gid == own && strcmp(pw->pw_name, name) != 0)
                remove_own = -1;
        endpwent();
    }
    for (int i = 0; i < n; i++)
        if (account_replace(ACCOUNT_GROUP, names[i], i == remove_own ? NULL : lines[i]) < 0)
            return -1;
    return 0;
}

int main(int argc, char **argv)
{
    int remove_home = 0;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-r") == 0) {
        remove_home = 1;
        i++;
    }
    if (argc - i != 1) {
        fprintf(stderr, "usage: userdel [-r] name\n");
        return 2;
    }
    const char *name = argv[i];
    if (getuid() != 0 || geteuid() != 0) {
        fprintf(stderr, "userdel: only root may remove accounts\n");
        return 1;
    }
    struct passwd *pw = getpwnam(name);
    if (!pw) {
        fprintf(stderr, "userdel: unknown account %s\n", name);
        return 1;
    }
    if (pw->pw_uid == 0) {
        fprintf(stderr, "userdel: root cannot be removed\n");
        return 1;
    }
    char home[256];
    snprintf(home, sizeof home, "%s", pw->pw_dir);
    gid_t gid = pw->pw_gid;
    if (account_replace(ACCOUNT_PASSWD, name, NULL) < 0 || account_replace(ACCOUNT_SHADOW, name, NULL) < 0 ||
        fix_groups(name, gid) < 0) {
        fprintf(stderr, "userdel: %s\n", strerror(errno));
        return 1;
    }
    if (remove_home && home[0] == '/' && strcmp(home, "/") != 0 && remove_tree(home) < 0 && errno != ENOENT) {
        fprintf(stderr, "userdel: %s: %s\n", home, strerror(errno));
        return 1;
    }
    return 0;
}
