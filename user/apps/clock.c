/* clock: the time of day (UTC) on a canvas, redrawn every second by a timer. */
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <gui/app.h>

static struct widget *canvas;

static int draw(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    painter_fill(p, 0, 0, w->w, w->h, 0x00ffffff);
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char buf[16];
    snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    int tw = painter_text_width(p, buf, -1);
    painter_text(p, (w->w - tw) / 2, (w->h - painter_text_height(p)) / 2, buf, 0x00000000);
    return 1;
}

static void tick(void *arg)
{
    widget_invalidate(canvas);
}

int main(void)
{
    struct app *app = app_create();
    if (!app)
        return 1;
    struct widget *win = app_window(app, 200, 100, "clock");
    if (!win)
        return 1;
    widget_set_padding(win, 0);
    canvas = canvas_new(win);
    widget_connect(canvas, "paint", draw, NULL);
    app_timer_add(app, 1000, 1, tick, NULL);
    app_run(app);
    app_destroy(app);
    return 0;
}
