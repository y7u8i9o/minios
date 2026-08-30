/* ln: create a hard link. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: ln target name\n");
        return 2;
    }
    if (link(argv[1], argv[2]) < 0) {
        fprintf(stderr, "ln: %s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    return 0;
}
