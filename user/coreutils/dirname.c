/* dirname: print the directory part of a path. */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: dirname path\n");
        return 2;
    }
    char *p = argv[1];
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/')
        p[--n] = '\0';
    char *slash = strrchr(p, '/');
    if (!slash) {
        puts(".");
        return 0;
    }
    while (slash > p && *slash == '/')
        slash--;
    slash[1] = '\0';
    if (strcmp(p, "") == 0)
        p = "/";
    puts(p);
    return 0;
}
