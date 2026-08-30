/* fortune: print a random saying from /etc/fortunes, where entries are
 * separated by lines containing a single %. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/etc/fortunes";
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return 1;
    }
    char **entries = NULL;
    int n = 0, cap = 0;
    char cur[2048] = "";
    char line[256];
    for (;;) {
        char *got = fgets(line, sizeof line, f);
        if (!got || strcmp(line, "%\n") == 0 || strcmp(line, "%") == 0) {
            if (cur[0]) {
                if (n == cap) {
                    cap = cap ? cap * 2 : 32;
                    entries = realloc(entries, (size_t)cap * sizeof *entries);
                }
                entries[n++] = strdup(cur);
                cur[0] = '\0';
            }
            if (!got)
                break;
            continue;
        }
        strncat(cur, line, sizeof cur - strlen(cur) - 1);
    }
    fclose(f);
    if (n == 0) {
        fprintf(stderr, "fortune: no entries\n");
        return 1;
    }
    srand((unsigned)uptime_ms() ^ (unsigned)getpid());
    fputs(entries[rand() % n], stdout);
    return 0;
}
