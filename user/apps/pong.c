/* pong: the left paddle follows w and s, the right paddle the arrow
 * keys. First to 7 points wins. Escape quits. A 16 ms timer drives the
 * game; the canvas draws the field. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gui/app.h>

#define PAD_H 60
#define PAD_W 8
#define BALL 8

static struct app *app;
static struct widget *canvas;
static int W = 480, H = 320;
static int ly, ry, bx, by, vx, vy, lscore, rscore;
static int lup, ldown, rup, rdown;
static int winner_ticks;

static void reset_ball(int dir)
{
    bx = W / 2 - BALL / 2;
    by = H / 2 - BALL / 2;
    vx = 3 * dir;
    vy = (rand() % 5) - 2;
    if (vy == 0)
        vy = 1;
}

static int on_paint(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    W = w->w;
    H = w->h;
    painter_fill(p, 0, 0, W, H, 0x00101820);
    for (int y = 0; y < H; y += 16)
        painter_fill(p, W / 2 - 1, y, 2, 8, 0x00405060);
    painter_fill(p, 10, ly, PAD_W, PAD_H, 0x00e0e0e0);
    painter_fill(p, W - 10 - PAD_W, ry, PAD_W, PAD_H, 0x00e0e0e0);
    painter_fill(p, bx, by, BALL, BALL, 0x00ffd040);
    char s[32];
    snprintf(s, sizeof s, "%d", lscore);
    painter_text(p, W / 2 - 40, 8, s, 0x00e0e0e0);
    snprintf(s, sizeof s, "%d", rscore);
    painter_text(p, W / 2 + 32, 8, s, 0x00e0e0e0);
    if (winner_ticks)
        painter_text(p, W / 2 - 60, H / 2 - 8, lscore == 7 ? "left player wins" : "right player wins", 0x00ffffff);
    return 1;
}

static void step(void *arg)
{
    if (winner_ticks) {
        if (--winner_ticks == 0)
            lscore = rscore = 0;
        widget_invalidate(canvas);
        return;
    }
    if (lup && ly > 0) ly -= 4;
    if (ldown && ly < H - PAD_H) ly += 4;
    if (rup && ry > 0) ry -= 4;
    if (rdown && ry < H - PAD_H) ry += 4;
    bx += vx;
    by += vy;
    if (by <= 0 || by >= H - BALL)
        vy = -vy;
    if (vx < 0 && bx <= 10 + PAD_W && bx >= 10 && by + BALL >= ly && by <= ly + PAD_H) {
        vx = -vx + 1;
        vy += (by + BALL / 2 - (ly + PAD_H / 2)) / 10;
    }
    if (vx > 0 && bx + BALL >= W - 10 - PAD_W && bx + BALL <= W - 10 && by + BALL >= ry && by <= ry + PAD_H) {
        vx = -vx - 1;
        vy += (by + BALL / 2 - (ry + PAD_H / 2)) / 10;
    }
    if (bx < 0) {
        rscore++;
        reset_ball(1);
    } else if (bx > W) {
        lscore++;
        reset_ball(-1);
    }
    if (lscore == 7 || rscore == 7)
        winner_ticks = 120;
    widget_invalidate(canvas);
}

static int on_keys(struct widget *w, void *args, void *arg)
{
    struct sig_key *k = args;
    int down = arg != NULL;
    switch (k->code) {
    case KEY_W: lup = down; break;
    case KEY_S: ldown = down; break;
    case KEY_UP: rup = down; break;
    case KEY_DOWN: rdown = down; break;
    case KEY_ESC: if (down) app_quit(app, 0); break;
    }
    return 1;
}

int main(void)
{
    app = app_create();
    if (!app)
        return 1;
    struct widget *win = app_window(app, W, H, "pong");
    if (!win)
        return 1;
    widget_set_padding(win, 0);
    canvas = canvas_new(win);
    canvas->focusable = 1;
    widget_connect(canvas, "paint", on_paint, NULL);
    widget_connect(canvas, "key", on_keys, (void *)1);
    widget_connect(canvas, "keyup", on_keys, NULL);
    widget_focus(canvas);
    srand((unsigned)uptime_ms());
    ly = ry = H / 2 - PAD_H / 2;
    reset_ball(1);
    app_timer_add(app, 16, 1, step, NULL);
    app_run(app);
    app_destroy(app);
    return 0;
}
