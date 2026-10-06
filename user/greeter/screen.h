#pragma once
/* The full screen windows of the greeter and of the screen locker lock
 * (docs/design/users.md, docs/design/lock.md). Both windows have the same
 * background, top bar, card and account rows. The background is the
 * desktop colour of /etc/desktop.conf with a gradient, or its wallpaper.
 * The screen locker links screen.c from this directory. */
#include <gui/app.h>

#define SCREEN_CARD_W 340
#define SCREEN_AVATAR 40
#define SCREEN_ROW_H  52

/* Read the desktop colour and the wallpaper from /etc/desktop.conf. */
void screen_load_background(void);

/* A vertical box that fills the window and paints the background. */
struct widget *screen_backdrop_new(struct widget *window);

/* The top bar of the backdrop. The bar shows host on the left and the
 * clock in the middle. The function returns the box on the right of the
 * bar. The caller adds its buttons to this box. The clock follows the
 * time once screen_clock_start has run. */
struct widget *screen_bar_new(struct widget *backdrop, const char *host);
void screen_clock_start(struct app *a);

/* The card in the middle of the backdrop: a vertical box with padding,
 * SCREEN_CARD_W pixels wide, above the vertical centre. */
struct widget *screen_card_new(struct widget *backdrop);

/* Paint the avatar, the full name and the account name of an account in
 * a row of h pixels at x, y. */
void screen_paint_account(struct painter *p, int x, int y, int h, const char *name, const char *full_name,
                          uint32_t text, uint32_t dim);

/* A row that shows one account and takes no focus. */
struct widget *screen_account_new(struct widget *parent, const char *name, const char *full_name);
