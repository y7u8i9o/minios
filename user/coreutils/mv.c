#include <stdio.h>
#include <string.h>
#include <errno.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: mv source dest\n");
        return 1;
    }
    if (rename(argv[1], argv[2]) < 0) {
        fprintf(stderr, "mv: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}
