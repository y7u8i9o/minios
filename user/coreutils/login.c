/* login: start a session on the terminal (login(1)).
 *
 *     login [name]
 *
 * init runs login as root on the console. login asks for an account name
 * and its password. An account without a password, such as root and user
 * on a new system, has to choose one before its session starts. login then
 * gives the terminal to the account, takes on its groups, gid and uid and
 * runs its shell as a login shell in its home with a fresh environment.
 * When the shell ends, init starts login again. */
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
/* The display of the console. The session user owns it like the terminal,
 * which lets startgui run the display server as that user. */
#define CONSOLE_DISPLAY "/dev/fb0"

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

/* Ask an account without a password for a new one until two entries
 * match, and store it. Returns 0, or -1 when the input ends or the
 * password cannot be stored. */
static int choose_password(const struct account *a)
{
    printf("The account %s has no password. Choose one now.\n", a->name);
    for (;;) {
        char first[128], second[128];
        if (account_read_password("New password: ", first, sizeof first) < 0 ||
            account_read_password("Retype new password: ", second, sizeof second) < 0)
            return -1;
        int same = strcmp(first, second) == 0;
        int r = 1;
        if (!first[0])
            printf("The password must not be empty.\n");
        else if (!same)
            printf("The passwords differ.\n");
        else
            r = account_set_password(a->name, first);
        memset(first, 0, sizeof first);
        memset(second, 0, sizeof second);
        if (r == 0) {
            printf("The password of %s is set.\n", a->name);
            return 0;
        }
        if (r < 0) {
            fprintf(stderr, "login: %s: %s\n", ACCOUNT_SHADOW, strerror(errno));
            return -1;
        }
    }
}

static void start_session(const struct account *a)
{
    /* init lets the session user power off. Without init, as in a test,
     * the request fails and nothing changes. */
    account_session((int)a->uid);
    /* The terminal and the display belong to the account for the
     * session. */
    if (isatty(0)) {
        fchown(0, a->uid, a->gid);
        fchmod(0, 0620);
    }
    chown(CONSOLE_DISPLAY, a->uid, a->gid);
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
    char *envp[] = { home, user, logname, shell, term, "PATH=/bin:/usr/bin:/usr/local/bin", NULL };
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
    /* Between sessions the terminal and the display belong to root
     * again. */
    if (isatty(0)) {
        fchown(0, 0, 0);
        fchmod(0, 0620);
    }
    chown(CONSOLE_DISPLAY, 0, 0);
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
        if (ok && !a.hash[0] && choose_password(&a) < 0)
            return 1;
        if (ok)
            start_session(&a);
        sleep(ATTEMPT_DELAY);
        printf("Login incorrect\n");
    }
}
