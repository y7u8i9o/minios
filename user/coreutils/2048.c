/* 2048: slide tiles with the arrow keys or wasd, q quits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ipc.h>

static int b[4][4], score;

static void spawn(void)
{
    int empty = 0;
    for (int i = 0; i < 16; i++)
        if (!b[i / 4][i % 4])
            empty++;
    if (!empty)
        return;
    int pick = rand() % empty;
    for (int i = 0; i < 16; i++) {
        if (b[i / 4][i % 4])
            continue;
        if (pick-- == 0) {
            b[i / 4][i % 4] = rand() % 10 == 0 ? 4 : 2;
            return;
        }
    }
}

/* Slide one row toward index 0. Returns 1 if anything moved. */
static int slide(int *v)
{
    int out[4] = { 0, 0, 0, 0 }, n = 0, moved = 0;
    for (int i = 0; i < 4; i++)
        if (v[i])
            out[n++] = v[i];
    for (int i = 0; i + 1 < n; i++) {
        if (out[i] == out[i + 1]) {
            out[i] *= 2;
            score += out[i];
            for (int k = i + 1; k + 1 < 4; k++)
                out[k] = out[k + 1];
            out[3] = 0;
            n--;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (v[i] != out[i])
            moved = 1;
        v[i] = out[i];
    }
    return moved;
}

static int move(int dir)   /* 0 left, 1 right, 2 up, 3 down */
{
    int moved = 0;
    for (int i = 0; i < 4; i++) {
        int v[4];
        for (int k = 0; k < 4; k++) {
            int kk = (dir == 1 || dir == 3) ? 3 - k : k;
            v[k] = dir < 2 ? b[i][kk] : b[kk][i];
        }
        moved |= slide(v);
        for (int k = 0; k < 4; k++) {
            int kk = (dir == 1 || dir == 3) ? 3 - k : k;
            if (dir < 2) b[i][kk] = v[k]; else b[kk][i] = v[k];
        }
    }
    return moved;
}

static int can_move(void)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            if (!b[y][x]) return 1;
            if (x < 3 && b[y][x] == b[y][x + 1]) return 1;
            if (y < 3 && b[y][x] == b[y + 1][x]) return 1;
        }
    return 0;
}

static void show(void)
{
    printf("\033[H\033[2J  2048    score %d\n\n", score);
    for (int y = 0; y < 4; y++) {
        printf("  +------+------+------+------+\n  |");
        for (int x = 0; x < 4; x++) {
            if (b[y][x]) printf(" %4d |", b[y][x]); else printf("      |");
        }
        printf("\n");
    }
    printf("  +------+------+------+------+\n\n  arrows or wasd move, q quits\n");
    fflush(stdout);
}

static int read_key(void)
{
    unsigned char c;
    if (read(0, &c, 1) != 1)
        return 'q';
    if (c != 27)
        return c;
    if (read(0, &c, 1) != 1 || c != '[' || read(0, &c, 1) != 1)
        return 0;
    return 0x100 + c;
}

int main(void)
{
    struct termios saved, raw;
    if (tcgetattr(0, &saved) < 0) {
        perror("2048");
        return 1;
    }
    raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(0, TCSANOW, &raw);
    srand((unsigned)uptime_ms());
    spawn();
    spawn();
    for (;;) {
        show();
        if (!can_move()) {
            printf("  no moves left, final score %d\n", score);
            break;
        }
        int k = read_key();
        int dir = -1;
        if (k == 'q') break;
        if (k == 0x100 + 'D' || k == 'a') dir = 0;
        if (k == 0x100 + 'C' || k == 'd') dir = 1;
        if (k == 0x100 + 'A' || k == 'w') dir = 2;
        if (k == 0x100 + 'B' || k == 's') dir = 3;
        if (dir >= 0 && move(dir))
            spawn();
    }
    tcsetattr(0, TCSANOW, &saved);
    return 0;
}
