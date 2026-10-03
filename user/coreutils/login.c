/* login: start a session on the terminal (login(1)).
 *
 *     login [name]
 *
 * init runs login as root on the console. login asks for an account name
 * and its password, unless the account has none, gives the terminal to the
 * account, takes on its groups, gid and uid and runs its shell as a login
 * shell in its home with a fresh environment. When the shell ends, init
 * starts login again. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <minios/account.h>

#define ATTEMPT_DELAY 2

struct account {
    char name[33];
    uid_t uid;
    gid_t gid;
    char home[256];
    char shell[256];
    char hash[128];
};

/* Look up name in the databases. Returns 0, or -1 when there is no such
 * account. */
static int find_account(const char *name, struct account *a)
{
    struct passwd *pw = getpwnam(name);
    if (!pw)
        return -1;
    snprintf(a->name, sizeof a->name, "%s", pw->pw_name);
    a->uid = pw->pw_uid;
    a->gid = pw->pw_gid;
    snprintf(a->home, sizeof a->home, "%s", pw->pw_dir[0] ? pw->pw_dir : "/");
    snprintf(a->shell, sizeof a->shell, "%s", pw->pw_shell[0] ? pw->pw_shell : "/bin/sh");
    struct spwd *sp = getspnam(a->name);
    /* An account without a shadow entry cannot log in. */
    snprintf(a->hash, sizeof a->hash, "%s", sp ? sp->sp_pwdp : "!");
    return 0;
}

static int read_line(char *buf, size_t size)
{
    if (!fgets(buf, (int)size, stdin))
        return -1;
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

static void start_session(const struct account *a)
{
    /* The terminal belongs to the account for the session. */
    if (isatty(0)) {
        fchown(0, a->uid, a->gid);
        fchmod(0, 0620);
    }
    if (initgroups(a->name, a->gid) < 0 || setgid(a->gid) < 0 || setuid(a->uid) < 0) {
        perror("login: cannot change the identity");
        exit(1);
    }
    if (chdir(a->home) < 0) {
        fprintf(stderr, "login: no home directory %s, using /\n", a->home);
        chdir("/");
    }
    static char home[300], user[64], logname[64], shell[300], term[64];
    snprintf(home, sizeof home, "HOME=%s", a->home);
    snprintf(user, sizeof user, "USER=%s", a->name);
    snprintf(logname, sizeof logname, "LOGNAME=%s", a->name);
    snprintf(shell, sizeof shell, "SHELL=%s", a->shell);
    const char *t = getenv("TERM");
    snprintf(term, sizeof term, "TERM=%s", t ? t : "minios");
    char *envp[] = { home, user, logname, shell, term, "PATH=/bin:/usr/local/bin", NULL };
    /* A leading "-" in argv[0] marks a login shell. */
    static char arg0[64];
    const char *base = strrchr(a->shell, '/');
    snprintf(arg0, sizeof arg0, "-%s", base ? base + 1 : a->shell);
    char *argv[] = { arg0, NULL };
    fflush(stdout);
    execve(a->shell, argv, envp);
    fprintf(stderr, "login: %s: %s\n", a->shell, strerror(errno));
    exit(1);
}

int main(int argc, char **argv)
{
    if (geteuid() != 0) {
        fprintf(stderr, "login: must be run by root\n");
        return 1;
    }
    /* Between sessions the terminal belongs to root again. */
    if (isatty(0)) {
        fchown(0, 0, 0);
        fchmod(0, 0620);
    }
    struct utsname u;
    const char *host = uname(&u) == 0 && u.nodename[0] ? u.nodename : "minios";
    char name[64];
    const char *given = argc > 1 ? argv[1] : NULL;
    for (;;) {
        if (given) {
            snprintf(name, sizeof name, "%s", given);
            given = NULL;
        } else {
            printf("\n%s login: ", host);
            fflush(stdout);
            if (read_line(name, sizeof name) < 0)
                return 1;
            if (!name[0])
                continue;
        }
        struct account a;
        int known = find_account(name, &a) == 0;
        char password[128] = "";
        /* An unknown name is asked for a password as well, which keeps the
         * existence of accounts hidden. */
        if (!known || a.hash[0]) {
            if (account_read_password("Password: ", password, sizeof password) < 0)
                return 1;
        }
        int ok = known && account_check(password, a.hash);
        memset(password, 0, sizeof password);
        if (ok)
            start_session(&a);
        sleep(ATTEMPT_DELAY);
        printf("Login incorrect\n");
    }
}
