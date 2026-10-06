/* checkpass: check the password of the caller (docs/design/lock.md).
 *
 *     checkpass
 *
 * checkpass is installed set user id root. Only root may read
 * /etc/shadow. checkpass reads one line from standard input, or from the
 * terminal without echo. checkpass compares the line with the password of
 * the account of the real uid. The exit status is 0 for the right
 * password, 1 for a wrong one and 2 for an error. A wrong password costs
 * FAILURE_DELAY seconds before checkpass exits. The delay slows down
 * guessing. The screen locker lock uses checkpass. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <shadow.h>
#include <minios/account.h>

#define FAILURE_DELAY 2

int main(int argc, char **argv)
{
    if (argc != 1) {
        fprintf(stderr, "usage: checkpass < password\n");
        return 2;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "checkpass: not installed set user id root\n");
        return 2;
    }
    struct passwd *pw = getpwuid(getuid());
    if (!pw) {
        fprintf(stderr, "checkpass: no account of uid %u\n", (unsigned)getuid());
        return 2;
    }
    char password[256];
    if (account_read_password("Password: ", password, sizeof password) < 0)
        password[0] = '\0';
    struct spwd *sp = getspnam(pw->pw_name);
    int ok = sp && account_check(password, sp->sp_pwdp);
    memset(password, 0, sizeof password);
    if (!ok) {
        sleep(FAILURE_DELAY);
        return 1;
    }
    return 0;
}
