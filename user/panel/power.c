/* The power menu of the panel (B4 of docs/plan/desktop-panel.md): the
 * button near the right end of the bar opens a popup with Log out, Restart
 * and Shut down. Log out ends the session: startgui ends it when the panel
 * exits with status 0. Restart and Shut down send reboot and poweroff to
 * init (init_request), which accepts them from the user of the graphical
 * session (docs/design/users.md). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <gui/i18n.h>
#include <minios/init.h>
#include "panel.h"

#define MENU_W 200
#define ROW_H 28
#define PAD 6
#define NROWS 3

enum { ROW_LOGOUT, ROW_RESTART, ROW_SHUTDOWN };

static struct canvas menu;
static struct wire_proxy *popup;
static int menu_open;
static int hover_row = -1;

int power_x(void)
{
    return screen_w - DESKTOP_BTN_W - 4 - POWER_BTN_W;
}

int power_is_open(void)
{
    return menu_open;
}

int power_owns(const struct wire_proxy *surface)
{
    return menu.surface && surface == menu.surface;
}

void power_draw_button(struct painter *p, int hovered)
{
    int x = power_x(), h = panel.lh;
    if (menu_open || hovered)
        painter_rounded(p, x, BUTTON_Y, POWER_BTN_W, h - 2 * BUTTON_Y, menu_open ? BUTTON_OPEN : BUTTON_HOVER,
                        0xffffffffu);
    const struct image *icon = panel_icon("power-off", PANEL_TEXT);
    if (icon)
        painter_image(p, x + (POWER_BTN_W - image_lw(icon)) / 2, (h - image_lh(icon)) / 2, icon);
}

static void draw_menu(void)
{
    static const char *const icons[NROWS] = { "app-logout", "restart", "power-off" };
    const char *titles[NROWS] = { _("Log out"), _("Restart"), _("Shut down") };
    struct painter p;
    canvas_painter(&p, &menu);
    painter_fill(&p, 0, 0, menu.lw, menu.lh, MENU_BG);
    painter_frame(&p, 0, 0, menu.lw, menu.lh, MENU_BORDER);
    for (int i = 0; i < NROWS; i++) {
        int y = PAD + i * ROW_H;
        if (i == hover_row)
            painter_rounded(&p, PAD, y, menu.lw - 2 * PAD, ROW_H, MENU_HOVER, 0xffffffffu);
        const struct image *icon = panel_icon(icons[i], MENU_ICON);
        if (icon)
            painter_image(&p, PAD + 8, y + (ROW_H - image_lh(icon)) / 2, icon);
        panel_label(&p, PAD + 32, y, menu.lw - 2 * PAD - 32, ROW_H, titles[i], MENU_TEXT, 0);
    }
    /* The account of the session right of Log out. */
    struct passwd *pw = getpwuid(getuid());
    if (pw) {
        int tw = painter_text_width(&p, pw->pw_name, -1);
        panel_label(&p, menu.lw - PAD - 8 - tw - 6, PAD, tw + 12, ROW_H, pw->pw_name, MENU_TEXT_DIM, 0);
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
    menu_open = 0;
    hover_row = -1;
}

static void on_popup_configure(void *user, struct wire_proxy *p, uint32_t serial, int32_t x, int32_t y, int32_t w,
                               int32_t h)
{
    popup_ack_configure(p, serial);
    popup_grab(popup, seat, press_serial);
    draw_menu();
    log_line("power menu opened");
}

static void on_popup_done(void *user, struct wire_proxy *p)
{
    teardown();
    draw_panel();
}

static const struct popup_listener popup_events = { on_popup_configure, on_popup_done };

void power_toggle(void)
{
    if (menu_open) {
        teardown();
        draw_panel();
        return;
    }
    int h = 2 * PAD + NROWS * ROW_H;
    if (canvas_create(&menu, MENU_W, h) < 0) {
        teardown();
        return;
    }
    struct wire_proxy *pos = shell_create_positioner(shell);
    positioner_set_size(pos, MENU_W, h);
    panel_place_popup(pos, power_x(), POWER_BTN_W, 1);
    popup = shell_get_popup(shell, menu.surface, panel.surface, pos);
    popup_add_listener(popup, &popup_events, NULL);
    positioner_destroy(pos);
    menu_open = 1;
    draw_panel();
    wire_display_flush(display);
}

static int row_at(int y)
{
    int i = (y - PAD) / ROW_H;
    return y >= PAD && i >= 0 && i < NROWS ? i : -1;
}

void power_pointer_motion(int x, int y)
{
    int row = row_at(y);
    if (row != hover_row) {
        hover_row = row;
        draw_menu();
    }
}

void power_pointer_button(uint32_t button, uint32_t state, int x, int y)
{
    if (button != 1 || !state)
        return;
    int row = row_at(y);
    if (row < 0)
        return;
    teardown();
    draw_panel();
    if (row == ROW_LOGOUT) {
        log_line("logout");
        exit(0);                        /* startgui ends the session when the panel exits */
    }
    const char *request = row == ROW_RESTART ? "reboot" : "poweroff";
    char reply[128];
    long n = init_request(request, reply, sizeof reply);
    if (n < 0)
        log_line("%s: cannot reach init", request);
    else
        log_line("%s: %.*s", request, (int)strcspn(reply, "\n"), reply);
}
