/* The calendar of the panel (B3 of docs/plan/desktop-panel.md): a click on
 * the clock opens a popup with a month. The header shows the month and the
 * year between the buttons for the previous and the next month. Below it
 * are the abbreviated weekdays, beginning with the first day of the week
 * of the locale (_NL_FIRST_WEEKDAY), and the days of the month in six
 * rows. Today is marked with the accent colour. The popup has the same
 * size for every month. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <langinfo.h>
#include "panel.h"

#define PAD 6
#define CELL_W 36                       /* the width of "Wed" with margins */
#define CELL_H 28
#define HEAD_H 32
#define DAYS_H 24
#define CAL_W (2 * PAD + 7 * CELL_W)
#define CAL_H (2 * PAD + HEAD_H + DAYS_H + 6 * CELL_H)
#define NAV_W 32                        /* the buttons of the header */

static struct canvas menu;
static struct wire_proxy *popup;
static int menu_open;
static int year, month;                 /* the month shown, month 0 to 11 */
static int nav_hover;                   /* -1 or 1 for the button under the pointer, else 0 */

int calendar_is_open(void)
{
    return menu_open;
}

int calendar_owns(const struct wire_proxy *surface)
{
    return menu.surface && surface == menu.surface;
}

static int first_weekday(void)
{
    int d = atoi(nl_langinfo(_NL_FIRST_WEEKDAY));
    return d >= 0 && d <= 6 ? d : 0;
}

/* The weekday of the first day of the shown month (0 for Sunday) and the
 * number of its days. */
static void month_layout(int *wday, int *days)
{
    struct tm tm = { .tm_year = year - 1900, .tm_mon = month, .tm_mday = 1, .tm_hour = 12 };
    mktime(&tm);
    *wday = tm.tm_wday;
    struct tm last = { .tm_year = year - 1900, .tm_mon = month + 1, .tm_mday = 0, .tm_hour = 12 };
    mktime(&last);
    *days = last.tm_mday;
}

static void draw_menu(void)
{
    struct painter p;
    canvas_painter(&p, &menu);
    painter_card(&p, 0, 0, menu.lw, menu.lh, 0);

    /* The header: the month without a day, as the nominative of Russian,
     * and the year. */
    char title[80];
    snprintf(title, sizeof title, "%s %d", nl_langinfo(ALTMON_1 + month), year);
    panel_label(&p, PAD + NAV_W, PAD, menu.lw - 2 * PAD - 2 * NAV_W, HEAD_H, title, menu_theme.color[TC_TEXT], 1);
    painter_button(&p, PAD, PAD, NAV_W, HEAD_H, PAINTER_FLAT | (nav_hover < 0 ? PAINTER_HOVER : 0));
    painter_button(&p, menu.lw - PAD - NAV_W, PAD, NAV_W, HEAD_H, PAINTER_FLAT | (nav_hover > 0 ? PAINTER_HOVER : 0));
    const struct image *back = panel_icon("back", menu_theme.color[TC_TEXT]), *forward = panel_icon("forward", menu_theme.color[TC_TEXT]);
    if (back)
        painter_image(&p, PAD + (NAV_W - image_lw(back)) / 2, PAD + (HEAD_H - image_lh(back)) / 2, back);
    if (forward)
        painter_image(&p, menu.lw - PAD - NAV_W + (NAV_W - image_lw(forward)) / 2,
                      PAD + (HEAD_H - image_lh(forward)) / 2, forward);

    int first = first_weekday();
    for (int c = 0; c < 7; c++)
        panel_label(&p, PAD + c * CELL_W, PAD + HEAD_H, CELL_W, DAYS_H, nl_langinfo(ABDAY_1 + (first + c) % 7),
                    menu_theme.color[TC_TEXT_DISABLED], 1);

    time_t now = time(NULL);
    struct tm today;
    localtime_r(&now, &today);
    int wday, days;
    month_layout(&wday, &days);
    int column = (wday - first + 7) % 7;
    for (int d = 1; d <= days; d++) {
        int slot = column + d - 1;
        int x = PAD + (slot % 7) * CELL_W, y = PAD + HEAD_H + DAYS_H + (slot / 7) * CELL_H;
        int is_today = today.tm_year + 1900 == year && today.tm_mon == month && today.tm_mday == d;
        if (is_today)
            painter_rounded(&p, x + 2, y + 2, CELL_W - 4, CELL_H - 4, menu_theme.color[TC_ACCENT], 0xffffffffu);
        char num[4];
        snprintf(num, sizeof num, "%d", d);
        panel_label(&p, x, y, CELL_W, CELL_H, num, is_today ? 0x00ffffff : menu_theme.color[TC_TEXT], 1);
    }
    canvas_commit(&menu);
}

static void teardown(void)
{
    if (popup) {
        popup_destroy(popup);
        popup = NULL;
    }
    if (menu.surface) {
        surface_destroy(menu.surface);
        canvas_release_buffer(&menu);
        memset(&menu, 0, sizeof menu);
    }
    if (menu_open)
        log_line("calendar closed");
    menu_open = 0;
    nav_hover = 0;
}

static void on_popup_configure(void *user, struct wire_proxy *p, uint32_t serial, int32_t x, int32_t y, int32_t w,
                               int32_t h)
{
    popup_ack_configure(p, serial);
    popup_grab(popup, seat, press_serial);
    draw_menu();
    log_line("calendar opened %04d-%02d", year, month + 1);
}

static void on_popup_done(void *user, struct wire_proxy *p)
{
    teardown();
    draw_panel();
}

static const struct popup_listener popup_events = { on_popup_configure, on_popup_done };

void calendar_toggle(void)
{
    if (menu_open) {
        teardown();
        draw_panel();
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    year = tm.tm_year + 1900;
    month = tm.tm_mon;
    if (canvas_create(&menu, CAL_W, CAL_H) < 0) {
        teardown();
        return;
    }
    struct wire_proxy *pos = shell_create_positioner(shell);
    positioner_set_size(pos, CAL_W, CAL_H);
    panel_place_popup(pos, clock_x(), CLOCK_W - 4, 1);
    popup = shell_get_popup(shell, menu.surface, panel.surface, pos);
    popup_add_listener(popup, &popup_events, NULL);
    positioner_destroy(pos);
    menu_open = 1;
    draw_panel();
    wire_display_flush(display);
}

/* The button of the header at (x, y): -1 back, 1 forward, else 0. */
static int nav_at(int x, int y)
{
    if (y < PAD || y >= PAD + HEAD_H)
        return 0;
    return x >= PAD && x < PAD + NAV_W ? -1 : x >= menu.lw - PAD - NAV_W && x < menu.lw - PAD ? 1 : 0;
}

void calendar_pointer_motion(int x, int y)
{
    int n = nav_at(x, y);
    if (n != nav_hover) {
        nav_hover = n;
        draw_menu();
    }
}

/* The buttons of the header move one month back or forward. */
void calendar_pointer_button(uint32_t button, uint32_t state, int x, int y)
{
    if (button != 1 || !state)
        return;
    int step = nav_at(x, y);
    if (!step)
        return;
    month += step;
    if (month < 0) {
        month = 11;
        year--;
    } else if (month > 11) {
        month = 0;
        year++;
    }
    log_line("calendar month %04d-%02d", year, month + 1);
    draw_menu();
}
