/* stty size: print the rows and columns of the terminal on standard input. */
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

int main(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "size") != 0) {
        fprintf(stderr, "usage: stty size\n");
        return 2;
    }
    struct winsize ws;
    if (ioctl(0, TIOCGWINSZ, &ws) < 0) {
        perror("stty");
        return 1;
    }
    printf("%u %u\n", ws.ws_row, ws.ws_col);
    return 0;
}
