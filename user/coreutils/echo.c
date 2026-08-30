#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int start = 1, newline = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        start = 2;
        newline = 0;
    }
    for (int i = start; i < argc; i++)
        printf("%s%s", argv[i], i + 1 < argc ? " " : "");
    if (newline)
        printf("\n");
    return 0;
}
