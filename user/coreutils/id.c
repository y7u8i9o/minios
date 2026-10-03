/* id: print the user and group ids of the caller or of an account
 * (id(1)). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <limits.h>

static void print_user(const char *label, uid_t uid)
{
    struct passwd *pw = getpwuid(uid);
    if (pw)
        printf("%s=%u(%s)", label, uid, pw->pw_name);
    else
        printf("%s=%u", label, uid);
}

static void print_group(const char *label, gid_t gid)
{
    struct group *gr = getgrgid(gid);
    if (gr)
        printf("%s=%u(%s)", label, gid, gr->gr_name);
    else
        printf("%s=%u", label, gid);
}

static void print_groups(const gid_t *groups, int n)
{
    printf(" groups=");
    for (int i = 0; i < n; i++) {
        struct group *gr = getgrgid(groups[i]);
        if (gr)
            printf("%s%u(%s)", i ? "," : "", groups[i], gr->gr_name);
        else
            printf("%s%u", i ? "," : "", groups[i]);
    }
}

static int usage(void)
{
    fprintf(stderr, "usage: id [-u | -g | -G] [-n] [user]\n");
    return 2;
}

int main(int argc, char **argv)
{
    char which = 0;
    int names = 0, opt;
    while ((opt = getopt(argc, argv, "ugGn")) != -1) {
        switch (opt) {
        case 'u': case 'g': case 'G': which = (char)opt; break;
        case 'n': names = 1; break;
        default: return usage();
        }
    }
    if (argc - optind > 1)
        return usage();

    uid_t ruid, euid;
    gid_t rgid, egid;
    gid_t groups[NGROUPS_MAX + 1];
    int ngroups;
    if (optind < argc) {
        struct passwd *pw = getpwnam(argv[optind]);
        if (!pw) {
            fprintf(stderr, "id: %s: no such user\n", argv[optind]);
            return 1;
        }
        ruid = euid = pw->pw_uid;
        rgid = egid = pw->pw_gid;
        char name[64];
        snprintf(name, sizeof name, "%s", pw->pw_name);
        ngroups = NGROUPS_MAX + 1;
        if (getgrouplist(name, rgid, groups, &ngroups) < 0)
            ngroups = NGROUPS_MAX + 1;
    } else {
        ruid = getuid();
        euid = geteuid();
        rgid = getgid();
        egid = getegid();
        /* The effective group comes first, as in getgrouplist. */
        groups[0] = egid;
        int n = getgroups(NGROUPS_MAX, groups + 1);
        ngroups = 1;
        for (int i = 1; i <= n; i++)
            if (groups[i] != egid)
                groups[ngroups++] = groups[i];
    }

    if (which == 'u' || which == 'g') {
        unsigned id = which == 'u' ? euid : egid;
        if (names) {
            struct passwd *pw = which == 'u' ? getpwuid(euid) : NULL;
            struct group *gr = which == 'g' ? getgrgid(egid) : NULL;
            if (pw || gr) {
                printf("%s\n", pw ? pw->pw_name : gr->gr_name);
                return 0;
            }
        }
        printf("%u\n", id);
        return 0;
    }
    if (which == 'G') {
        for (int i = 0; i < ngroups; i++) {
            struct group *gr = names ? getgrgid(groups[i]) : NULL;
            if (gr)
                printf("%s%s", i ? " " : "", gr->gr_name);
            else
                printf("%s%u", i ? " " : "", groups[i]);
        }
        printf("\n");
        return 0;
    }
    print_user("uid", ruid);
    printf(" ");
    print_group("gid", rgid);
    if (euid != ruid) {
        printf(" ");
        print_user("euid", euid);
    }
    if (egid != rgid) {
        printf(" ");
        print_group("egid", egid);
    }
    print_groups(groups, ngroups);
    printf("\n");
    return 0;
}
