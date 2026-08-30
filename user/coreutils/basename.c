/* basename: strip directories and an optional suffix. */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: basename path [suffix]\n");
        return 2;
    }
    char *p = argv[1];
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/')
        p[--n] = '\0';
    char *base = strrchr(p, '/');
    base = base && base[1] ? base + 1 : (base ? base : p);
    if (argc > 2) {
        size_t bl = strlen(base), sl = strlen(argv[2]);
        if (sl < bl && strcmp(base + bl - sl, argv[2]) == 0)
            base[bl - sl] = '\0';
    }
    puts(base);
    return 0;
}
