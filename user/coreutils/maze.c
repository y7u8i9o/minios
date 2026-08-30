/* maze: a random maze carved with a depth first walk. maze [width height]. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int W = 30, H = 12;
static unsigned char *grid;   /* (2W+1) x (2H+1) cells, 1 = wall */

static int gw(void) { return 2 * W + 1; }

static void carve(int cx, int cy)
{
    int dirs[4] = { 0, 1, 2, 3 };
    for (int i = 3; i > 0; i--) {
        int j = rand() % (i + 1), t = dirs[i];
        dirs[i] = dirs[j];
        dirs[j] = t;
    }
    for (int i = 0; i < 4; i++) {
        int dx = dirs[i] == 0 ? 1 : dirs[i] == 1 ? -1 : 0;
        int dy = dirs[i] == 2 ? 1 : dirs[i] == 3 ? -1 : 0;
        int nx = cx + dx, ny = cy + dy;
        if (nx < 0 || ny < 0 || nx >= W || ny >= H)
            continue;
        if (!grid[(2 * ny + 1) * gw() + 2 * nx + 1])
            continue;
        grid[(2 * ny + 1) * gw() + 2 * nx + 1] = 0;
        grid[(2 * cy + 1 + dy) * gw() + 2 * cx + 1 + dx] = 0;
        carve(nx, ny);
    }
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        W = atoi(argv[1]);
        H = atoi(argv[2]);
    }
    if (W < 2 || H < 2 || W > 100 || H > 60)
        return 2;
    grid = malloc((size_t)gw() * (2 * H + 1));
    for (int i = 0; i < gw() * (2 * H + 1); i++)
        grid[i] = 1;
    srand(argc > 3 ? (unsigned)atoi(argv[3]) : (unsigned)uptime_ms());
    grid[1 * gw() + 1] = 0;
    carve(0, 0);
    grid[1 * gw() + 0] = 0;                       /* entrance */
    grid[(2 * H - 1) * gw() + 2 * W] = 0;         /* exit */
    for (int y = 0; y < 2 * H + 1; y++) {
        for (int x = 0; x < gw(); x++)
            fputs(grid[y * gw() + x] ? "#" : " ", stdout);
        putchar('\n');
    }
    return 0;
}
