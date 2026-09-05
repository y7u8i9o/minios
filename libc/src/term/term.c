#include <term.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>

int term_use_color(int fd)
{
    const char *term = getenv("TERM");
    return isatty(fd) && term && *term && strcmp(term, "dumb") && !getenv("NO_COLOR");
}

int term_columns(int fd)
{
    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col)
        return ws.ws_col;
    const char *value = getenv("COLUMNS");
    int n = value ? atoi(value) : 0;
    return n > 0 && n <= 65535 ? n : 80;
}

const char *term_sgr(int color)
{
    static _Thread_local char sequence[16];
    snprintf(sequence, sizeof sequence, "\033[%dm", color >= 0 && color <= 107 ? color : 0);
    return sequence;
}
