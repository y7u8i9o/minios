/* ps: print the process table from /dev/proc, with the account name of
 * each process in a USER column. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <pwd.h>

/* The account name of the uid column, or the number itself. */
static const char *user_name(const char *uid_text)
{
    static char last_uid[16], last_name[32];
    if (strcmp(uid_text, last_uid) != 0) {
        char *end;
        unsigned long uid = strtoul(uid_text, &end, 10);
        struct passwd *pw = *end ? NULL : getpwuid((uid_t)uid);
        snprintf(last_name, sizeof last_name, "%s", pw ? pw->pw_name : uid_text);
        snprintf(last_uid, sizeof last_uid, "%s", uid_text);
    }
    return last_name;
}

int main(void)
{
    FILE *file = fopen("/dev/proc", "r");
    if (!file) {
        fprintf(stderr, "ps: /dev/proc: %s\n", strerror(errno));
        return 1;
    }
    char *line = NULL;
    size_t capacity = 0;
    int header = 1;
    while (getline(&line, &capacity, file) >= 0) {
        char *columns[8], *p = line;
        int count = 0;
        while (*p && count < 8) {
            p += strspn(p, " \t\r\n");
            if (!*p)
                break;
            columns[count++] = p;
            p += strcspn(p, count == 8 ? "\r\n" : " \t\r\n");
            if (*p)
                *p++ = 0;
        }
        if (count != 8)
            continue;
        printf("%5s %5s %5s %-8s %-8s %8s %8s %s\n", columns[0], columns[1], columns[2],
               header ? "USER" : user_name(columns[6]), columns[3], columns[4], columns[5], columns[7]);
        header = 0;
    }
    int status = ferror(file) || ferror(stdout);
    free(line);
    fclose(file);
    return status;
}
