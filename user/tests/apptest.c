/* M21 boot test client: two windows driven by one application loop, a
 * repeating timer, a watched pipe descriptor, signals and damage logs. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <gui/app.h>

static struct app *app;
static struct timer *timer;
static int ticks;
static int pipefd[2];

static int on_click(struct widget *w, void *args, void *arg)
{
    printf("apptest: clicked\n");
    fflush(stdout);
    return 1;
}

static int on_change(struct widget *w, void *args, void *arg)
{
    struct sig_change *c = args;
    printf("apptest: changed '%s'\n", c->text);
    fflush(stdout);
    return 0;
}

static int on_select(struct widget *w, void *args, void *arg)
{
    printf("apptest: selected %d\n", ((struct sig_select *)args)->index);
    fflush(stdout);
    return 0;
}

static int on_close(struct widget *w, void *args, void *arg)
{
    printf("apptest: close %s\n", (const char *)arg);
    fflush(stdout);
    return 0;
}

static void on_tick(void *arg)
{
    ticks++;
    printf("apptest: tick %d\n", ticks);
    fflush(stdout);
    if (ticks == 3) {
        app_timer_remove(app, timer);
        write(pipefd[1], "x", 1);
    }
}

static void on_pipe(int fd, int revents, void *arg)
{
    char c;
    read(fd, &c, 1);
    printf("apptest: pipe readable\n");
    fflush(stdout);
}

int main(void)
{
    app = app_create();
    if (!app) {
        printf("apptest: cannot connect\n");
        return 1;
    }
    app_set_damage_log(app, 1);
    struct widget *two = app_window(app, 200, 120, "two");
    label_new(two, "second window");
    widget_connect(two, "close", on_close, "two");
    struct widget *one = app_window(app, 400, 300, "one");
    struct widget *b = button_new(one, "&Click");
    widget_set_hint(b, 0, 30);
    widget_connect(b, "clicked", on_click, NULL);
    struct widget *f = textfield_new(one, "");
    widget_connect(f, "changed", on_change, NULL);
    struct widget *l = listview_new(one);
    for (int i = 0; i < 5; i++) {
        char s[16];
        snprintf(s, sizeof s, "entry %d", i);
        listview_add(l, s);
    }
    widget_connect(l, "selected", on_select, NULL);
    widget_connect(one, "close", on_close, "one");
    if (pipe(pipefd) < 0)
        return 1;
    app_watch_fd(app, pipefd[0], POLLIN, on_pipe, NULL);
    timer = app_timer_add(app, 200, 1, on_tick, NULL);
    printf("apptest: started\n");
    fflush(stdout);
    int code = app_run(app);
    app_destroy(app);
    printf("apptest: exit %d\n", code);
    fflush(stdout);
    return 0;
}
