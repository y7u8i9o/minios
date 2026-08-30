/* rev: reverse each line. */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "r") : stdin;
    if (!f) {
        perror("rev");
        return 1;
    }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n')
            n--;
        for (size_t i = n; i > 0; i--)
            putchar(line[i - 1]);
        putchar('\n');
    }
    return 0;
}
