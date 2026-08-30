#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

int main(int argc, char **argv)
{
    int status = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: mkdir dir...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i], 0755) < 0) {
            fprintf(stderr, "mkdir: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
