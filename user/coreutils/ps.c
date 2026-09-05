/* ps: print the process table from /dev/proc. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>

int main(void)
{
    FILE *file = fopen("/dev/proc", "r");
    if (!file) {
        fprintf(stderr, "ps: /dev/proc: %s\n", strerror(errno));
        return 1;
    }
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, file) >= 0) {
        char *columns[7], *p = line;
        int count = 0;
        while (*p && count < 7) {
            p += strspn(p, " \t\r\n");
            if (!*p)
                break;
            columns[count++] = p;
            p += strcspn(p, count == 7 ? "\r\n" : " \t\r\n");
            if (*p)
                *p++ = 0;
        }
        if (count == 7)
            printf("%5s %5s %5s %-8s %8s %8s %s\n", columns[0], columns[1], columns[2], columns[3],
                   columns[4], columns[5], columns[6]);
    }
    int status = ferror(file) || ferror(stdout);
    free(line);
    fclose(file);
    return status;
}
