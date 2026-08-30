/* more: page through a file one screen at a time. Space or enter shows the
 * next page, q quits. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "r") : stdin;
    if (!f) {
        perror("more");
        return 1;
    }
    int rows = 24;
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2)
        rows = ws.ws_row;
    /* Keys come from the terminal even when the text comes from a pipe. */
    int tty = open("/dev/console", O_RDONLY);
    if (isatty(0))
        tty = 0;
    struct termios saved, raw;
    int have_tty = tty >= 0 && tcgetattr(tty, &saved) == 0;
    if (have_tty) {
        raw = saved;
        raw.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(tty, TCSANOW, &raw);
    }
    char line[1024];
    int shown = 0;
    while (fgets(line, sizeof line, f)) {
        fputs(line, stdout);
        if (++shown < rows - 1)
            continue;
        printf("--More--");
        fflush(stdout);
        char c = 'q';
        if (have_tty)
            while (read(tty, &c, 1) == 1 && c != ' ' && c != '\n' && c != 'q')
                ;
        printf("\r        \r");
        if (c == 'q')
            break;
        shown = c == '\n' ? rows - 2 : 0;
    }
    if (have_tty)
        tcsetattr(tty, TCSANOW, &saved);
    return 0;
}
