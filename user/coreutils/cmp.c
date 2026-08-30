/* cmp: compare two files byte by byte. */
#include <stdio.h>
#include <string.h>
#include <errno.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: cmp file1 file2\n");
        return 2;
    }
    FILE *a = fopen(argv[1], "r"), *b = fopen(argv[2], "r");
    if (!a || !b) {
        fprintf(stderr, "cmp: %s: %s\n", !a ? argv[1] : argv[2], strerror(errno));
        return 2;
    }
    long pos = 1, line = 1;
    for (;;) {
        int ca = fgetc(a), cb = fgetc(b);
        if (ca == EOF && cb == EOF)
            return 0;
        if (ca == EOF || cb == EOF) {
            printf("cmp: EOF on %s\n", ca == EOF ? argv[1] : argv[2]);
            return 1;
        }
        if (ca != cb) {
            printf("%s %s differ: byte %ld, line %ld\n", argv[1], argv[2], pos, line);
            return 1;
        }
        if (ca == '\n')
            line++;
        pos++;
    }
}
