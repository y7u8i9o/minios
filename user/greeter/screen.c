/* The top bar and the card of the greeter and of the screen locker
 * (screen.h). */
#include "screen.h"
#include <time.h>

static struct widget *clock_label;

struct widget *screen_bar_new(struct widget *backdrop, const char *host)
{
    struct widget *bar = toolbar_new(backdrop);
    widget_set_padding(bar, 4);
    struct widget *host_label = label_new(bar, host);
    widget_set_stretch(host_label, 1, 0);
    clock_label = label_new(bar, "");
    toolbar_set_center(bar, clock_label);
    /* The box on the right is transparent. The centred clock can extend
     * into the box and remains visible. */
    struct widget *right = box_new(bar, 0);
    right->transparent = 1;
    widget_set_padding(right, 0);
    widget_set_stretch(right, 1, 0);
    struct widget *gap = spacer_new(right);
    widget_set_stretch(gap, 1, 0);
    return right;
}

static void tick(void *arg)
{
    char text[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(text, sizeof text, "%a %d %b  %H:%M", &tm);
    widget_set_text(clock_label, text);
}

void screen_clock_start(struct app *a)
{
    tick(NULL);
    app_timer_add(a, 10000, 1, tick, NULL);
}

struct widget *screen_card_new(struct widget *backdrop)
{
    struct widget *top = spacer_new(backdrop);
    widget_set_stretch(top, 0, 1);
    struct widget *card = card_new(backdrop, SCREEN_CARD_W);
    struct widget *bottom = spacer_new(backdrop);
    widget_set_stretch(bottom, 0, 2);
    return card;
}
