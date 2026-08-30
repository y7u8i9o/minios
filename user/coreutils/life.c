/* life: Conway's game of life. -g N runs N generations (default 100),
 * -w W -h H sets the size, -q prints only the final generation, -r seeds
 * randomly instead of with an R-pentomino. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int W = 60, H = 20;
static unsigned char *cells, *next;

static int at(int x, int y)
{
    x = (x + W) % W;
    y = (y + H) % H;
    return cells[y * W + x];
}

static void step(void)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if (dx || dy)
                        n += at(x + dx, y + dy);
            next[y * W + x] = n == 3 || (n == 2 && at(x, y));
        }
    }
    unsigned char *t = cells;
    cells = next;
    next = t;
}

static void show(int gen)
{
    printf("\033[H");
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++)
            putchar(cells[y * W + x] ? '#' : '.');
        putchar('\n');
    }
    int alive = 0;
    for (int i = 0; i < W * H; i++)
        alive += cells[i];
    printf("generation %d, %d alive    \n", gen, alive);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int gens = 100, quiet = 0, random = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) gens = atoi(argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) W = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) H = atoi(argv[++i]);
        else if (strcmp(argv[i], "-q") == 0) quiet = 1;
        else if (strcmp(argv[i], "-r") == 0) random = 1;
        else {
            fprintf(stderr, "usage: life [-g gens] [-w width] [-h height] [-q] [-r]\n");
            return 2;
        }
    }
    if (W < 5 || H < 5 || W > 400 || H > 200)
        return 2;
    cells = calloc((size_t)W * H, 1);
    next = calloc((size_t)W * H, 1);
    if (random) {
        srand((unsigned)uptime_ms());
        for (int i = 0; i < W * H; i++)
            cells[i] = rand() % 4 == 0;
    } else {
        int cx = W / 2, cy = H / 2;
        cells[cy * W + cx] = cells[cy * W + cx + 1] = 1;
        cells[(cy - 1) * W + cx - 1] = cells[(cy - 1) * W + cx] = 1;
        cells[(cy + 1) * W + cx] = 1;
    }
    if (!quiet)
        printf("\033[2J");
    for (int g = 0; g < gens; g++) {
        if (!quiet) {
            show(g);
            sleep_ms(80);
        }
        step();
    }
    show(gens);
    return 0;
}
