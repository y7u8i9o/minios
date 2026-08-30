/* snake: arrow keys steer, eat the * to grow, q quits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>

#define MAXLEN 1000

static int W = 40, H = 18;
static int sx[MAXLEN], sy[MAXLEN], len, dx = 1, dy;
static int fx, fy, score;

static void put(int x, int y, char c)
{
    printf("\033[%d;%dH%c", y + 2, x + 2, c);
}

static void place_food(void)
{
    for (;;) {
        fx = rand() % W;
        fy = rand() % H;
        int clash = 0;
        for (int i = 0; i < len; i++)
            if (sx[i] == fx && sy[i] == fy)
                clash = 1;
        if (!clash)
            break;
    }
    put(fx, fy, '*');
}

static int read_key(int timeout_ms)
{
    struct pollfd pf = { 0, POLLIN, 0 };
    if (poll(&pf, 1, timeout_ms) <= 0)
        return 0;
    unsigned char c;
    if (read(0, &c, 1) != 1)
        return 0;
    if (c != 27)
        return c;
    if (read(0, &c, 1) != 1 || c != '[')
        return 27;
    if (read(0, &c, 1) != 1)
        return 27;
    return 0x100 + c;
}

int main(void)
{
    struct termios saved, raw;
    if (tcgetattr(0, &saved) < 0) {
        perror("snake");
        return 1;
    }
    raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(0, TCSANOW, &raw);
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 20 && ws.ws_row >= 8) {
        W = ws.ws_col - 2;
        H = ws.ws_row - 4;
    }
    srand((unsigned)uptime_ms());
    printf("\033[2J\033[H+");
    for (int x = 0; x < W; x++) putchar('-');
    printf("+");
    for (int y = 0; y < H; y++)
        printf("\033[%d;1H|\033[%d;%dH|", y + 2, y + 2, W + 2);
    printf("\033[%d;1H+", H + 2);
    for (int x = 0; x < W; x++) putchar('-');
    printf("+");
    len = 3;
    for (int i = 0; i < len; i++) {
        sx[i] = W / 2 - i;
        sy[i] = H / 2;
        put(sx[i], sy[i], i ? 'o' : '@');
    }
    place_food();
    int delay = 150, alive = 1;
    while (alive) {
        printf("\033[%d;1Hscore %d   arrows move, q quits", H + 3, score);
        fflush(stdout);
        int k = read_key(delay);
        if (k == 'q') break;
        if ((k == 0x100 + 'A' || k == 'w') && dy != 1) { dx = 0; dy = -1; }
        if ((k == 0x100 + 'B' || k == 's') && dy != -1) { dx = 0; dy = 1; }
        if ((k == 0x100 + 'C' || k == 'd') && dx != -1) { dx = 1; dy = 0; }
        if ((k == 0x100 + 'D' || k == 'a') && dx != 1) { dx = -1; dy = 0; }
        int nx = sx[0] + dx, ny = sy[0] + dy;
        if (nx < 0 || ny < 0 || nx >= W || ny >= H) {
            alive = 0;
            break;
        }
        for (int i = 0; i < len - 1; i++)
            if (sx[i] == nx && sy[i] == ny)
                alive = 0;
        if (!alive)
            break;
        int grow = nx == fx && ny == fy;
        if (!grow)
            put(sx[len - 1], sy[len - 1], ' ');
        else if (len < MAXLEN)
            len++;
        for (int i = len - 1; i > 0; i--) {
            sx[i] = sx[i - 1];
            sy[i] = sy[i - 1];
        }
        sx[0] = nx;
        sy[0] = ny;
        put(sx[1], sy[1], 'o');
        put(nx, ny, '@');
        if (grow) {
            score += 10;
            if (delay > 60)
                delay -= 5;
            place_food();
        }
    }
    printf("\033[%d;1H%s score %d.                     \n", H + 3, alive ? "Quit," : "Game over,", score);
    tcsetattr(0, TCSANOW, &saved);
    return 0;
}
