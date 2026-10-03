/* passwd: change the password or the full name of an account (passwd(1)).
 *
 *     passwd [-d] [-n full-name] [name]
 *
 * passwd is set user id root. Without a name it changes the account of
 * the caller's real uid. Only root may name another account and remove a
 * password with -d. A caller other than root confirms the current
 * password first, unless the account has none. On a terminal the
 * passwords are typed without echo, otherwise they are read one per line
 * from standard input: the current one when it is asked for, then the new
 * one twice. -n sets the full name, the fifth field of /etc/passwd. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pwd.h>
#include <shadow.h>
#include <minios/account.h>

#define FAILURE_DELAY 2

static int usage(void)
{
    fprintf(stderr, "usage: passwd [-d] [-n full-name] [name]\n");
    return 2;
}

/* A numeric field of /etc/shadow, empty for -1. */
static const char *field(long v, char *buf, size_t size)
{
    if (v < 0)
        buf[0] = '\0';
    else
        snprintf(buf, size, "%ld", v);
    return buf;
}

static int set_hash(const char *name, const char *hash)
{
    struct spwd *sp = getspnam(name);
    char line[512], a[24], b[24], c[24], d[24], e[24], f[24];
    if (sp)
        snprintf(line, sizeof line, "%s:%s:%ld:%s:%s:%s:%s:%s:%s", name, hash, account_today(),
                 field(sp->sp_min, a, sizeof a), field(sp->sp_max, b, sizeof b), field(sp->sp_warn, c, sizeof c),
                 field(sp->sp_inact, d, sizeof d), field(sp->sp_expire, e, sizeof e),
                 sp->sp_flag == (unsigned long)-1 ? "" : field((long)sp->sp_flag, f, sizeof f));
    else
        snprintf(line, sizeof line, "%s:%s:%ld::::::", name, hash, account_today());
    return account_replace(ACCOUNT_SHADOW, name, line);
}

static int set_full_name(const char *name, const char *full)
{
    if (strpbrk(full, ":\n")) {
        fprintf(stderr, "passwd: the full name may not contain ':' or a newline\n");
        return -1;
    }
    struct passwd *pw = getpwnam(name);
    if (!pw)
        return -1;
    char line[768];
    snprintf(line, sizeof line, "%s:%s:%u:%u:%s:%s:%s", pw->pw_name, pw->pw_passwd, pw->pw_uid, pw->pw_gid, full,
             pw->pw_dir, pw->pw_shell);
    return account_replace(ACCOUNT_PASSWD, name, line);
}

int main(int argc, char **argv)
{
    int remove = 0, opt;
    const char *full = NULL;
    while ((opt = getopt(argc, argv, "dn:")) != -1) {
        if (opt == 'd')
            remove = 1;
        else if (opt == 'n')
            full = optarg;
        else
            return usage();
    }
    if (argc - optind > 1)
        return usage();
    if (geteuid() != 0) {
        fprintf(stderr, "passwd: not installed set user id root\n");
        return 1;
    }
    int root = getuid() == 0;
    char name[33];
    if (optind < argc) {
        snprintf(name, sizeof name, "%s", argv[optind]);
    } else {
        struct passwd *self = getpwuid(getuid());
        if (!self) {
            fprintf(stderr, "passwd: no account with uid %u\n", getuid());
            return 1;
        }
        snprintf(name, sizeof name, "%s", self->pw_name);
    }
    struct passwd *pw = getpwnam(name);
    if (!pw) {
        fprintf(stderr, "passwd: unknown account %s\n", name);
        return 1;
    }
    if (!root && pw->pw_uid != getuid()) {
        fprintf(stderr, "passwd: only root may change another account\n");
        return 1;
    }
    if (remove && !root) {
        fprintf(stderr, "passwd: only root may remove a password\n");
        return 1;
    }
    if (full) {
        if (set_full_name(name, full) < 0) {
            fprintf(stderr, "passwd: %s: %s\n", ACCOUNT_PASSWD, strerror(errno));
            return 1;
        }
        return 0;
    }
    if (remove) {
        if (set_hash(name, "") < 0) {
            fprintf(stderr, "passwd: %s: %s\n", ACCOUNT_SHADOW, strerror(errno));
            return 1;
        }
        printf("passwd: password of %s removed\n", name);
        return 0;
    }

    char current[128] = "", first[128] = "", second[128] = "";
    if (!root) {
        struct spwd *sp = getspnam(name);
        char hash[128];
        snprintf(hash, sizeof hash, "%s", sp ? sp->sp_pwdp : "!");
        if (hash[0] && account_read_password("Current password: ", current, sizeof current) < 0)
            return 1;
        int ok = account_check(current, hash);
        memset(current, 0, sizeof current);
        if (!ok) {
            sleep(FAILURE_DELAY);
            fprintf(stderr, "passwd: authentication failure\n");
            return 1;
        }
    }
    if (account_read_password("New password: ", first, sizeof first) < 0 ||
        account_read_password("Retype new password: ", second, sizeof second) < 0)
        return 1;
    int status = 1;
    char hash[128];
    if (strcmp(first, second) != 0)
        fprintf(stderr, "passwd: the passwords differ\n");
    else if (!first[0])
        fprintf(stderr, "passwd: the password is empty, -d removes a password\n");
    else if (account_hash(first, hash, sizeof hash) < 0 || set_hash(name, hash) < 0)
        fprintf(stderr, "passwd: %s: %s\n", ACCOUNT_SHADOW, strerror(errno));
    else {
        printf("passwd: password of %s changed\n", name);
        status = 0;
    }
    memset(first, 0, sizeof first);
    memset(second, 0, sizeof second);
    return status;
}
