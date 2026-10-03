/* groups: print the group names of the caller or of accounts (id(1)). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <limits.h>

static void print(const gid_t *groups, int n)
{
    for (int i = 0; i < n; i++) {
        struct group *gr = getgrgid(groups[i]);
        if (gr)
            printf("%s%s", i ? " " : "", gr->gr_name);
        else
            printf("%s%u", i ? " " : "", groups[i]);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    gid_t groups[NGROUPS_MAX + 1];
    if (argc < 2) {
        gid_t egid = getegid();
        groups[0] = egid;
        int n = getgroups(NGROUPS_MAX, groups + 1), count = 1;
        for (int i = 1; i <= n; i++)
            if (groups[i] != egid)
                groups[count++] = groups[i];
        print(groups, count);
        return 0;
    }
    int status = 0;
    for (int a = 1; a < argc; a++) {
        struct passwd *pw = getpwnam(argv[a]);
        if (!pw) {
            fprintf(stderr, "groups: %s: no such user\n", argv[a]);
            status = 1;
            continue;
        }
        char name[64];
        snprintf(name, sizeof name, "%s", pw->pw_name);
        int n = NGROUPS_MAX + 1;
        if (getgrouplist(name, pw->pw_gid, groups, &n) < 0)
            n = NGROUPS_MAX + 1;
        if (argc > 2)
            printf("%s : ", name);
        print(groups, n);
    }
    return status;
}
