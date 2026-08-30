/* cut: select fields (-d DELIM -f LIST) or characters (-c LIST) of each
 * line. LIST is comma separated numbers or ranges such as 2-4 or 3-. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int wanted[1024];
static char delim = '\t';
static int mode;    /* 'f' or 'c' */

static void parse_list(const char *s)
{
    while (*s) {
        int a = (int)strtol(s, (char **)&s, 10), b = a;
        if (*s == '-') {
            s++;
            b = (*s >= '0' && *s <= '9') ? (int)strtol(s, (char **)&s, 10) : 1023;
        }
        for (int i = a; i <= b && i < 1024; i++)
            if (i > 0)
                wanted[i] = 1;
        if (*s == ',')
            s++;
        else
            break;
    }
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            delim = argv[++i][0];
        else if (strncmp(argv[i], "-d", 2) == 0)
            delim = argv[i][2];
        else if ((strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "-c") == 0) && i + 1 < argc) {
            mode = argv[i][1];
            parse_list(argv[++i]);
        } else if (strncmp(argv[i], "-f", 2) == 0 || strncmp(argv[i], "-c", 2) == 0) {
            mode = argv[i][1];
            parse_list(argv[i] + 2);
        } else {
            fprintf(stderr, "usage: cut -d DELIM -f LIST | cut -c LIST [file]\n");
            return 2;
        }
    }
    if (!mode) {
        fprintf(stderr, "cut: -f or -c required\n");
        return 2;
    }
    FILE *f = i < argc ? fopen(argv[i], "r") : stdin;
    if (!f) {
        perror("cut");
        return 1;
    }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n')
            line[--n] = '\0';
        if (mode == 'c') {
            for (size_t k = 0; k < n; k++)
                if (k + 1 < 1024 && wanted[k + 1])
                    putchar(line[k]);
        } else {
            int field = 1, printed = 0;
            char *p = line;
            while (p) {
                char *next = strchr(p, delim);
                if (next)
                    *next = '\0';
                if (field < 1024 && wanted[field]) {
                    if (printed++)
                        putchar(delim);
                    fputs(p, stdout);
                }
                field++;
                p = next ? next + 1 : NULL;
            }
        }
        putchar('\n');
    }
    return 0;
}
