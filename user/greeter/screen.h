#pragma once
/* The full screen windows of the greeter and of the screen locker lock
 * (docs/design/users.md, docs/design/lock.md). Both windows have the same
 * backdrop, top bar, card and account rows. The classes come from libgui
 * (docs/design/widgets.md). The screen locker links screen.c from this
 * directory. */
#include <gui/app.h>

#define SCREEN_CARD_W 340

/* The top bar of the backdrop. The bar shows host on the left and the
 * clock in the middle. The function returns the box on the right of the
 * bar. The caller adds its buttons to this box. The clock follows the
 * time once screen_clock_start has run. */
struct widget *screen_bar_new(struct widget *backdrop, const char *host);
void screen_clock_start(struct app *a);

/* The card in the middle of the backdrop, SCREEN_CARD_W pixels wide,
 * above the vertical centre. */
struct widget *screen_card_new(struct widget *backdrop);
