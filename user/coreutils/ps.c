/* ps: print the process table from /dev/proc, with the account name of
 * each process in a USER column. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <pwd.h>
#include <minios/proctab.h>

/* The account name of uid, or the number itself. */
static const char *user_name(unsigned uid)
{
    static unsigned last_uid = (unsigned)-1;
    static char last_name[32];
    if (uid != last_uid) {
        struct passwd *pw = getpwuid((uid_t)uid);
        if (pw)
            snprintf(last_name, sizeof last_name, "%s", pw->pw_name);
        else
            snprintf(last_name, sizeof last_name, "%u", uid);
        last_uid = uid;
    }
    return last_name;
}

int main(void)
{
    static struct proc_entry rows[256];
    int n = proc_table_read(rows, 256);
    if (n < 0) {
        fprintf(stderr, "ps: /dev/proc: %s\n", strerror(-n));
        return 1;
    }
    printf("%5s %5s %5s %-8s %-8s %8s %8s %s\n", "PID", "PPID", "PGID", "USER", "STATE", "TIME", "RSS", "NAME");
    for (int i = 0; i < n; i++)
        printf("%5d %5d %5d %-8s %-8s %8lu %8lu %s\n", (int)rows[i].pid, (int)rows[i].ppid, (int)rows[i].pgid,
               user_name(rows[i].uid), rows[i].state, rows[i].ticks, rows[i].rss_kib, rows[i].name);
    return ferror(stdout) ? 1 : 0;
}
