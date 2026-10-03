/* useradd: create an account (useradd(1)).
 *
 *     useradd [-u uid] [-g group] [-G group,...] [-c full-name] [-d home]
 *             [-s shell] [-M] name
 *
 * Only root may create accounts. The account receives the lowest free uid
 * from 1000 on unless -u gives one, and a group of its own name with the
 * same number unless -g names an existing group. -G adds it to further
 * groups. The home is /home/NAME unless -d names another, made from
 * /etc/skel unless -M is given. The password is locked until passwd(1)
 * sets one. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <minios/account.h>

static int usage(void)
{
    fprintf(stderr, "usage: useradd [-u uid] [-g group] [-G group,...] [-c full-name] [-d home] [-s shell] [-M] name\n");
    return 2;
}

static int uid_taken(unsigned id)
{
    return getpwuid(id) != NULL;
}

static int gid_taken(unsigned id)
{
    return getgrgid(id) != NULL;
}

/* Add name to the member list of the group called group. */
static int add_member(const char *group, const char *name)
{
    struct group *gr = getgrnam(group);
    if (!gr) {
        fprintf(stderr, "useradd: unknown group %s\n", group);
        return -1;
    }
    char line[1024];
    int n = snprintf(line, sizeof line, "%s:%s:%u:", gr->gr_name, gr->gr_passwd, gr->gr_gid);
    int first = 1;
    for (char **m = gr->gr_mem; *m; m++) {
        if (strcmp(*m, name) == 0)
            return 0;
        n += snprintf(line + n, sizeof line - (size_t)n, "%s%s", first ? "" : ",", *m);
        first = 0;
    }
    snprintf(line + n, sizeof line - (size_t)n, "%s%s", first ? "" : ",", name);
    char key[64];
    snprintf(key, sizeof key, "%s", gr->gr_name);
    return account_replace(ACCOUNT_GROUP, key, line);
}

static int fail(const char *what)
{
    fprintf(stderr, "useradd: %s: %s\n", what, strerror(errno));
    return 1;
}

int main(int argc, char **argv)
{
    const char *uid_text = NULL, *group = NULL, *extra = NULL, *full = "", *home_opt = NULL;
    const char *shell = "/bin/sh";
    int make_home = 1, opt;
    while ((opt = getopt(argc, argv, "u:g:G:c:d:s:M")) != -1) {
        switch (opt) {
        case 'u': uid_text = optarg; break;
        case 'g': group = optarg; break;
        case 'G': extra = optarg; break;
        case 'c': full = optarg; break;
        case 'd': home_opt = optarg; break;
        case 's': shell = optarg; break;
        case 'M': make_home = 0; break;
        default: return usage();
        }
    }
    if (argc - optind != 1)
        return usage();
    const char *name = argv[optind];
    if (getuid() != 0 || geteuid() != 0) {
        fprintf(stderr, "useradd: only root may create accounts\n");
        return 1;
    }
    if (!account_name_valid(name)) {
        fprintf(stderr, "useradd: invalid name %s\n", name);
        return 1;
    }
    if (strpbrk(full, ":\n") || (home_opt && strpbrk(home_opt, ":\n")) || strpbrk(shell, ":\n")) {
        fprintf(stderr, "useradd: fields may not contain ':' or a newline\n");
        return 1;
    }
    if (getpwnam(name)) {
        fprintf(stderr, "useradd: account %s exists\n", name);
        return 1;
    }
    unsigned uid;
    if (uid_text) {
        char *end;
        uid = (unsigned)strtoul(uid_text, &end, 10);
        if (*end || !uid_text[0]) {
            fprintf(stderr, "useradd: invalid uid %s\n", uid_text);
            return 1;
        }
        if (uid_taken(uid)) {
            fprintf(stderr, "useradd: uid %u is taken\n", uid);
            return 1;
        }
    } else {
        /* The account's own group takes the same number, so both must be
         * free. */
        for (uid = ACCOUNT_FIRST_ID; uid_taken(uid) || (!group && gid_taken(uid)); uid++)
            ;
    }
    unsigned gid;
    char line[768];
    if (group) {
        struct group *gr = getgrnam(group);
        if (!gr) {
            fprintf(stderr, "useradd: unknown group %s\n", group);
            return 1;
        }
        gid = gr->gr_gid;
    } else {
        if (getgrnam(name)) {
            fprintf(stderr, "useradd: group %s exists, name it with -g\n", name);
            return 1;
        }
        for (gid = uid; gid_taken(gid); gid++)
            ;
        snprintf(line, sizeof line, "%s:x:%u:", name, gid);
        if (account_replace(ACCOUNT_GROUP, name, line) < 0)
            return fail(ACCOUNT_GROUP);
    }
    char home[256];
    if (home_opt)
        snprintf(home, sizeof home, "%s", home_opt);
    else
        snprintf(home, sizeof home, "/home/%s", name);
    snprintf(line, sizeof line, "%s:x:%u:%u:%s:%s:%s", name, uid, gid, full, home, shell);
    if (account_replace(ACCOUNT_PASSWD, name, line) < 0)
        return fail(ACCOUNT_PASSWD);
    snprintf(line, sizeof line, "%s:!:%ld::::::", name, account_today());
    if (account_replace(ACCOUNT_SHADOW, name, line) < 0)
        return fail(ACCOUNT_SHADOW);
    if (extra) {
        char groups[256];
        snprintf(groups, sizeof groups, "%s", extra);
        for (char *g = strtok(groups, ","); g; g = strtok(NULL, ","))
            if (add_member(g, name) < 0)
                return 1;
    }
    if (make_home && account_make_home(home, uid, gid) < 0)
        return fail(home);
    return 0;
}
