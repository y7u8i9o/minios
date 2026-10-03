/* whoami: print the account name of the effective uid (id(1)). */
#include <stdio.h>
#include <unistd.h>
#include <pwd.h>

int main(void)
{
    uid_t uid = geteuid();
    struct passwd *pw = getpwuid(uid);
    if (!pw) {
        fprintf(stderr, "whoami: no account with uid %u\n", uid);
        return 1;
    }
    printf("%s\n", pw->pw_name);
    return 0;
}
