/* matrix: falling character rain until a key is pressed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>

int main(void)
{
    int cols = 80, rows = 25;
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
    if (cols > 200) cols = 200;
    struct termios saved, raw;
    int have_tty = tcgetattr(0, &saved) == 0;
    if (have_tty) {
        raw = saved;
        raw.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(0, TCSANOW, &raw);
    }
    srand((unsigned)uptime_ms());
    int head[200], len[200];
    for (int c = 0; c < cols; c++) {
        head[c] = -(rand() % (rows * 2));
        len[c] = 4 + rand() % 10;
    }
    printf("\033[2J");
    for (int frame = 0; ; frame++) {
        for (int c = 0; c < cols; c += 1) {
            int y = head[c];
            if (y >= 0 && y < rows) {
                char ch = (char)(33 + rand() % 93);
                printf("\033[%d;%dH%c", y + 1, c + 1, ch);
            }
            int tail = y - len[c];
            if (tail >= 0 && tail < rows)
                printf("\033[%d;%dH ", tail + 1, c + 1);
            head[c]++;
            if (tail >= rows) {
                head[c] = -(rand() % rows);
                len[c] = 4 + rand() % 10;
            }
        }
        printf("\033[%d;%dH", rows, cols);
        fflush(stdout);
        struct pollfd pf = { 0, POLLIN, 0 };
        if (poll(&pf, 1, 60) > 0) {
            char c;
            read(0, &c, 1);
            break;
        }
    }
    if (have_tty)
        tcsetattr(0, TCSANOW, &saved);
    printf("\033[2J\033[H");
    return 0;
}
