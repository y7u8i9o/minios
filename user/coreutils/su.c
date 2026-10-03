/* su: run a shell or a command as another account (su(1)).
 *
 *     su [-] [-c command] [name]
 *
 * su is set user id root. Without a name it switches to root. A caller
 * that is not root gives the password of the target account, unless the
 * account has none. On a terminal the password is typed without echo,
 * otherwise it is the first line of standard input, read even for an
 * account without a password, which lets a program such as the settings
 * window pass it through a pipe without knowing whether one is set. "-" starts a login
 * shell in the home of the account with a fresh environment, otherwise the
 * environment is kept with HOME, SHELL, USER and LOGNAME of the account. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <minios/account.h>

#define FAILURE_DELAY 2

static int usage(void)
{
    fprintf(stderr, "usage: su [-] [-c command] [name]\n");
    return 2;
}

int main(int argc, char **argv)
{
    int login = 0;
    const char *command = NULL, *target = "root";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0 || strcmp(argv[i], "-l") == 0) {
            login = 1;
        } else if (strcmp(argv[i], "-c") == 0) {
            if (++i == argc)
                return usage();
            command = argv[i];
        } else if (argv[i][0] == '-') {
            return usage();
        } else if (i == argc - 1) {
            target = argv[i];
        } else {
            return usage();
        }
    }
    if (geteuid() != 0) {
        fprintf(stderr, "su: not installed set user id root\n");
        return 1;
    }
    struct passwd *pw = getpwnam(target);
    if (!pw) {
        fprintf(stderr, "su: unknown account %s\n", target);
        return 1;
    }
    char name[33], home[256], shell[256];
    snprintf(name, sizeof name, "%s", pw->pw_name);
    snprintf(home, sizeof home, "%s", pw->pw_dir[0] ? pw->pw_dir : "/");
    snprintf(shell, sizeof shell, "%s", pw->pw_shell[0] ? pw->pw_shell : "/bin/sh");
    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;

    if (getuid() != 0) {
        struct spwd *sp = getspnam(name);
        char hash[128];
        snprintf(hash, sizeof hash, "%s", sp ? sp->sp_pwdp : "!");
        char password[128] = "";
        if ((hash[0] || !isatty(0)) && account_read_password("Password: ", password, sizeof password) < 0)
            password[0] = '\0';
        int ok = account_check(password, hash);
        memset(password, 0, sizeof password);
        if (!ok) {
            sleep(FAILURE_DELAY);
            fprintf(stderr, "su: authentication failure\n");
            return 1;
        }
    }

    if (initgroups(name, gid) < 0 || setgid(gid) < 0 || setuid(uid) < 0) {
        perror("su: cannot change the identity");
        return 1;
    }
    static char e_home[300], e_user[64], e_logname[64], e_shell[300], e_term[64];
    snprintf(e_home, sizeof e_home, "HOME=%s", home);
    snprintf(e_user, sizeof e_user, "USER=%s", name);
    snprintf(e_logname, sizeof e_logname, "LOGNAME=%s", name);
    snprintf(e_shell, sizeof e_shell, "SHELL=%s", shell);
    if (login) {
        const char *t = getenv("TERM");
        snprintf(e_term, sizeof e_term, "TERM=%s", t ? t : "minios");
        static char *fresh[] = { e_home, e_user, e_logname, e_shell, e_term, "PATH=/bin:/usr/local/bin", NULL };
        environ = fresh;
        if (chdir(home) < 0)
            chdir("/");
    } else {
        setenv("HOME", home, 1);
        setenv("USER", name, 1);
        setenv("LOGNAME", name, 1);
        setenv("SHELL", shell, 1);
    }
    static char arg0[64];
    const char *base = strrchr(shell, '/');
    snprintf(arg0, sizeof arg0, "%s%s", login ? "-" : "", base ? base + 1 : shell);
    char *args[] = { arg0, command ? "-c" : NULL, (char *)command, NULL };
    fflush(stdout);
    execv(shell, args);
    fprintf(stderr, "su: %s: %s\n", shell, strerror(errno));
    return 1;
}
