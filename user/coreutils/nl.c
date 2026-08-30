/* nl: number lines. */
#include <stdio.h>

int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "r") : stdin;
    if (!f) {
        perror("nl");
        return 1;
    }
    char line[1024];
    int n = 0;
    while (fgets(line, sizeof line, f))
        printf("%6d\t%s", ++n, line);
    return 0;
}
