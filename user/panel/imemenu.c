/* The input method menu of the panel (I5, docs/design/ime.md): the label
 * left of the mixer button opens a popup with the keyboard layout and the
 * engines of the input method daemon, and a mark on the current one.  The
 * methods come from the ime_control interface of the compositor, and a
 * click selects one through it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui/i18n.h>
#include "panel.h"
#include "ime-client.h"

#define MENU_W 220
#define ROW_H 28
#define PAD 6
#define MAX_METHODS 12

static struct wire_proxy *control;
static struct canvas menu;
static struct wire_proxy *popup;
static int menu_open;
static char names[MAX_METHODS][32], labels[MAX_METHODS][16], titles[MAX_METHODS][64];
static int nmethods;
static char current[32];

int imemenu_x(void)
{
    return mixer_button_x() - INPUT_W - 4;
}

int imemenu_owns(const struct wire_proxy *surface)
{
    return menu.surface && surface == menu.surface;
}

/* The titles of the known methods in the language of the panel. */
static const char *title_of(int i)
{
    if (strcmp(names[i], "layout") == 0)
        return _("Keyboard layout");
    if (strcmp(names[i], "pinyin") == 0)
        return _("Chinese (Pinyin)");
    if (strcmp(names[i], "japanese") == 0)
        return _("Japanese");
    return titles[i];
}

static void on_engines(void *user, struct wire_proxy *c, const char *list)
{
    nmethods = 0;
    while (*list && nmethods < MAX_METHODS) {
        const char *end = strchr(list, '\n');
        size_t n = end ? (size_t)(end - list) : strlen(list);
        char line[160];
        if (n >= sizeof line)
            n = sizeof line - 1;
        memcpy(line, list, n);
        line[n] = '\0';
        char *label = strchr(line, '\t'), *title = label ? strchr(label + 1, '\t') : NULL;
        if (label && title) {
            *label++ = '\0';
            *title++ = '\0';
            strlcpy(names[nmethods], line, sizeof names[0]);
            strlcpy(labels[nmethods], label, sizeof labels[0]);
            strlcpy(titles[nmethods], title, sizeof titles[0]);
            nmethods++;
        }
        list += n + (end ? 1 : 0);
    }
}

static void on_current(void *user, struct wire_proxy *c, const char *name, const char *label)
{
    strlcpy(current, name, sizeof current);
}

static const struct ime_control_listener control_events = { on_engines, on_current };

void imemenu_bind(struct wire_proxy *manager)
{
    control = input_method_manager_get_control(manager);
    ime_control_add_listener(control, &control_events, NULL);
}

static void draw_menu(void)
{
    struct painter p;
    canvas_painter(&p, &menu);
    painter_fill(&p, 0, 0, menu.lw, menu.lh, MENU_BG);
    painter_frame(&p, 0, 0, menu.lw, menu.lh, MENU_BORDER);
    for (int i = 0; i < nmethods; i++) {
        int y = PAD + i * ROW_H;
        int mine = strcmp(names[i], current) == 0;
        if (mine)
            painter_rounded(&p, PAD, y, menu.lw - 2 * PAD, ROW_H, MENU_HOVER, 0xffffffffu);
        panel_label(&p, PAD, y, 36, ROW_H, labels[i][0] ? labels[i] : "A", mine ? MENU_TEXT : MENU_TEXT_DIM, 1);
        panel_label(&p, PAD + 36, y, menu.lw - 2 * PAD - 36, ROW_H, title_of(i), MENU_TEXT, 0);
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
}

static void on_popup_configure(void *user, struct wire_proxy *p, uint32_t serial, int32_t x, int32_t y, int32_t w,
                               int32_t h)
{
    popup_ack_configure(p, serial);
    popup_grab(popup, seat, press_serial);
    draw_menu();
    log_line("input method menu opened, %d methods", nmethods);
}

static void on_popup_done(void *user, struct wire_proxy *p)
{
    teardown();
    draw_panel();
}

static const struct popup_listener popup_events = { on_popup_configure, on_popup_done };

void imemenu_toggle(void)
{
    if (menu_open || !control || !nmethods) {
        teardown();
        return;
    }
    int h = 2 * PAD + nmethods * ROW_H;
    if (canvas_create(&menu, MENU_W, h) < 0) {
        teardown();
        return;
    }
    struct wire_proxy *pos = shell_create_positioner(shell);
    positioner_set_size(pos, MENU_W, h);
    positioner_set_anchor_rect(pos, imemenu_x(), 4, INPUT_W, 1);
    positioner_set_anchor(pos, POS_TOP_RIGHT);       /* the top right of the label */
    positioner_set_gravity(pos, POS_TOP_LEFT);       /* extends up and to the left */
    popup = shell_get_popup(shell, menu.surface, panel.surface, pos);
    popup_add_listener(popup, &popup_events, NULL);
    positioner_destroy(pos);
    menu_open = 1;
    wire_display_flush(display);
}

void imemenu_pointer_button(uint32_t button, uint32_t state, int x, int y)
{
    if (button != 1 || !state)
        return;
    int i = (y - PAD) / ROW_H;
    if (y < PAD || i < 0 || i >= nmethods)
        return;
    log_line("input method %s selected", names[i]);
    ime_control_select(control, names[i]);
    teardown();
    draw_panel();
}
