/* Menu bar with drop down menus shown as window popups. */
#include <gui/app.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../intl.h"

#define MENU_PAD 10
#define ITEM_H_EXTRA 8

/* ---- items and menus ---- */

/* accel_label writes the name of the accelerator of an item, such as
 * "Ctrl+S", into buf, or an empty string when the item has none. */
static void accel_label(const struct widget *w, char *buf, size_t size)
{
    static const char row1[] = "qwertyuiop", row2[] = "asdfghjkl", row3[] = "zxcvbnm", digits[] = "1234567890";
    int k = w->accel_key;
    char key[32] = "";
    buf[0] = '\0';
    if (k >= KEY_Q && k < KEY_Q + 10) snprintf(key, sizeof key, "%c", row1[k - KEY_Q] - 32);
    else if (k >= KEY_A && k < KEY_A + 9) snprintf(key, sizeof key, "%c", row2[k - KEY_A] - 32);
    else if (k >= KEY_Z && k < KEY_Z + 7) snprintf(key, sizeof key, "%c", row3[k - KEY_Z] - 32);
    else if (k >= KEY_1 && k <= KEY_0) snprintf(key, sizeof key, "%c", digits[k - KEY_1]);
    else if (k >= KEY_F1 && k <= KEY_F10) snprintf(key, sizeof key, "F%d", k - KEY_F1 + 1);
    else if (k == KEY_DELETE) snprintf(key, sizeof key, "%s", _("Del"));
    else return;
    snprintf(buf, size, "%s%s%s%s", w->accel_mods & WMOD_CTRL ? _("Ctrl+") : "", w->accel_mods & WMOD_ALT ? _("Alt+") : "",
             w->accel_mods & WMOD_SHIFT ? _("Shift+") : "", key);
}

static void item_measure(struct widget *w, struct size_hint *h)
{
    const struct theme *t = widget_theme(w);
    if (w->value == 1) {                /* separator */
        h->pref_h = h->min_h = 7;
        h->pref_w = 20;
        return;
    }
    char accel[48], caption[256];
    accel_label(w, accel, sizeof accel);
    painter_mnemonic_strip(widget_text(w), caption, sizeof caption);
    h->pref_w = widget_text_width(w, NULL, caption, -1) + 2 * MENU_PAD + 24;
    if (accel[0])
        h->pref_w += widget_text_width(w, NULL, accel, -1) + 24;
    h->pref_h = h->min_h = t->font->height + ITEM_H_EXTRA;
}

const struct widget_class menuitem_class = { "menuitem", sizeof(struct widget), item_measure, NULL, NULL, NULL, NULL };
const struct widget_class menu_class = { "menu", sizeof(struct widget), NULL, NULL, NULL, NULL, NULL };

/* ---- the drop down popup ---- */

struct dropdown {
    struct widget w;
    struct widget *menu;
    struct widget *bar;
    int hover;
    int *accel_w;               /* the widths of the accelerator labels */
};

void widget_measure(struct widget *w);

static void dropdown_measure(struct widget *w, struct size_hint *h)
{
    struct dropdown *d = (struct dropdown *)w;
    int mw = 80, mh = 2, n = 0;
    for (struct widget *it = d->menu->first; it; it = it->next)
        n++;
    free(d->accel_w);
    d->accel_w = calloc((size_t)(n ? n : 1), sizeof *d->accel_w);
    n = 0;
    for (struct widget *it = d->menu->first; it; it = it->next, n++) {
        char accel[48];
        accel_label(it, accel, sizeof accel);
        if (d->accel_w && accel[0])
            d->accel_w[n] = widget_text_width(w, NULL, accel, -1);
        widget_measure(it);
        if (it->measured.pref_w > mw) mw = it->measured.pref_w;
        mh += it->measured.pref_h;
    }
    h->pref_w = mw;
    h->pref_h = mh;
}

static void dropdown_paint(struct widget *w, struct painter *p)
{
    struct dropdown *d = (struct dropdown *)w;
    const struct theme *t = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_FIELD]);
    painter_frame(p, 0, 0, w->w, w->h, t->color[TC_BORDER]);
    int y = 1, i = 0;
    for (struct widget *it = d->menu->first; it; it = it->next, i++) {
        int ih = it->measured.pref_h;
        if (it->value == 1) {
            painter_line(p, 4, y + ih / 2, w->w - 5, y + ih / 2, t->color[TC_BORDER]);
        } else {
            if (i == d->hover && it->enabled)
                painter_fill(p, 1, y, w->w - 2, ih, t->color[TC_HIGHLIGHT]);
            int x = MENU_PAD;
            if (it->icon) {
                painter_icon(p, 4, y + (ih - image_lh(it->icon)) / 2, it->icon, !it->enabled);
                x = 24;
            }
            painter_mnemonic_text(p, x, y + ITEM_H_EXTRA / 2, widget_text(it),
                                  t->color[it->enabled ? TC_TEXT : TC_TEXT_DISABLED]);
            char accel[48];
            accel_label(it, accel, sizeof accel);
            if (accel[0] && d->accel_w)
                painter_text(p, w->w - MENU_PAD - d->accel_w[i], y + ITEM_H_EXTRA / 2, accel, t->color[TC_TEXT_DISABLED]);
        }
        y += ih;
    }
}

static int item_at(struct dropdown *d, int py)
{
    int y = 1, i = 0;
    for (struct widget *it = d->menu->first; it; it = it->next, i++) {
        if (py >= y && py < y + it->measured.pref_h)
            return it->value == 1 ? -1 : i;
        y += it->measured.pref_h;
    }
    return -1;
}

static struct widget *item_n(struct dropdown *d, int n)
{
    struct widget *it = d->menu->first;
    while (it && n-- > 0)
        it = it->next;
    return it;
}

/* The next item that can be chosen, searched from n in the direction
 * step. The search skips separators and disabled items. The function
 * returns n when no such item exists. */
static int step_item(struct dropdown *d, int n, int count, int step)
{
    for (int k = 1; k <= count; k++) {
        int i = ((n + step * k) % count + count) % count;
        if (n < 0 && step < 0)
            i = ((count - k) % count + count) % count;
        struct widget *it = item_n(d, i);
        if (it && it->value != 1 && it->enabled)
            return i;
    }
    return n;
}

static void activate(struct dropdown *d, int n)
{
    struct widget *it = item_n(d, n);
    if (!it || it->value == 1 || !it->enabled)
        return;
    struct widget *bar = d->bar;
    window_popup_close(d->w.window);
    if (bar) {
        bar->value = -1;
        widget_invalidate(bar);
    }
    struct sig_click c = { 1, 0, 0 };
    widget_emit(it, "clicked", &c);
}

static int dropdown_event(struct widget *w, struct event *e)
{
    struct dropdown *d = (struct dropdown *)w;
    int n = 0;
    for (struct widget *it = d->menu->first; it; it = it->next)
        n++;
    switch (e->type) {
    case EV_MOUSE_MOVE: {
        int h = item_at(d, e->y);
        if (h != d->hover) {
            d->hover = h;
            widget_invalidate(w);
        }
        return 1;
    }
    case EV_MOUSE_DOWN:
        activate(d, item_at(d, e->y));
        return 1;
    case EV_KEY_DOWN:
        if (e->code == KEY_DOWN || e->code == KEY_UP) {
            int next = step_item(d, d->hover, n, e->code == KEY_DOWN ? 1 : -1);
            if (next != d->hover) {
                d->hover = next;
                widget_invalidate(w);
            }
            return 1;
        }
        if (e->ch == '\n') { activate(d, d->hover); return 1; }
        if ((e->code == KEY_LEFT || e->code == KEY_RIGHT) && d->bar) {
            struct widget *bar = d->bar;
            int count = 0;
            for (struct widget *m = bar->first; m; m = m->next)
                count++;
            menubar_open(bar, (bar->value + (e->code == KEY_RIGHT ? 1 : count - 1)) % count);
            return 1;
        }
        return 1;
    default:
        return 0;
    }
}

static void dropdown_destroy(struct widget *w)
{
    free(((struct dropdown *)w)->accel_w);
}

static const struct widget_class dropdown_class = { "dropdown", sizeof(struct dropdown), dropdown_measure, NULL,
                                                    dropdown_paint, dropdown_event, dropdown_destroy };

/* ---- menu bar ---- */

/* The menu bar measures the widths of its titles once per measurement. */
struct menubar {
    struct widget w;
    int *title_w, ntitles;
};

static int title_width(const struct widget *bar, const struct widget *m)
{
    const struct menubar *mb = (const struct menubar *)bar;
    int i = 0;
    for (const struct widget *c = bar->first; c && c != m; c = c->next)
        i++;
    return i < mb->ntitles ? mb->title_w[i] : 0;
}

static void menubar_measure(struct widget *w, struct size_hint *h)
{
    struct menubar *mb = (struct menubar *)w;
    const struct theme *t = widget_theme(w);
    int tw = 0, n = 0;
    for (struct widget *m = w->first; m; m = m->next)
        n++;
    int *widths = realloc(mb->title_w, (size_t)(n ? n : 1) * sizeof *widths);
    if (!widths)
        return;
    mb->title_w = widths;
    mb->ntitles = n;
    n = 0;
    for (struct widget *m = w->first; m; m = m->next) {
        char caption[256];
        painter_mnemonic_strip(widget_text(m), caption, sizeof caption);
        widths[n] = widget_text_width(w, NULL, caption, -1) + 2 * MENU_PAD;
        tw += widths[n++];
    }
    h->pref_w = tw;
    h->min_w = 20;
    h->pref_h = h->min_h = t->font->height + 8;
}

static void menubar_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_WINDOW]);
    painter_line(p, 0, w->h - 1, w->w - 1, w->h - 1, t->color[TC_BORDER]);
    int x = 0, i = 0;
    for (struct widget *m = w->first; m; m = m->next, i++) {
        int tw = title_width(w, m);
        if (i == w->value)
            painter_fill(p, x, 0, tw, w->h - 1, t->color[TC_HIGHLIGHT]);
        painter_mnemonic_text(p, x + MENU_PAD, 4, widget_text(m), t->color[TC_TEXT]);
        x += tw;
    }
}

void menubar_open(struct widget *bar, int index)
{
    struct widget *m = bar->first;
    int x = 0, i = 0;
    for (; m && i < index; m = m->next, i++)
        x += title_width(bar, m);
    if (index < 0 || !m) {
        window_popup_close(bar->window);
        bar->value = -1;
        widget_invalidate(bar);
        return;
    }
    bar->value = index;
    widget_invalidate(bar);
    struct dropdown *d = (struct dropdown *)widget_new(&dropdown_class, NULL);
    d->menu = m;
    d->bar = bar;
    d->hover = -1;
    d->w.app = bar->app;
    d->w.window = bar->window;
    struct size_hint h = { 0 };
    dropdown_measure(&d->w, &h);
    int ax, ay;
    widget_abs(bar, &ax, &ay);
    window_popup_open(bar->window, &d->w, ax + x, ay + bar->h, h.pref_w, h.pref_h);
    widget_focus(&d->w);
}

static int menubar_event(struct widget *w, struct event *e)
{
    if (e->type == EV_MOUSE_DOWN && (e->button & 1)) {
        int x = 0, i = 0;
        for (struct widget *m = w->first; m; m = m->next, i++) {
            int tw = title_width(w, m);
            if (e->x >= x && e->x < x + tw) {
                menubar_open(w, i == w->value ? -1 : i);
                return 1;
            }
            x += tw;
        }
        return 1;
    }
    return 0;
}

static void menubar_destroy(struct widget *w)
{
    free(((struct menubar *)w)->title_w);
}

const struct widget_class menubar_class = { "menubar", sizeof(struct menubar), menubar_measure, NULL, menubar_paint,
                                            menubar_event, menubar_destroy };

struct widget *menubar_new(struct widget *parent)
{
    struct widget *w = widget_new(&menubar_class, parent);
    if (w)
        w->value = -1;
    return w;
}

struct widget *menu_new(struct widget *bar, const char *title)
{
    struct widget *m = widget_new(&menu_class, bar);
    if (m) {
        m->visible = 0;             /* only the title is drawn, by the bar */
        widget_set_text(m, title);
    }
    return m;
}

struct widget *menu_add(struct widget *menu, const char *text, const char *icon)
{
    struct widget *it = widget_new(&menuitem_class, menu);
    if (it) {
        it->visible = 0;
        widget_set_text(it, text);
        if (icon)
            it->icon = icon_get(icon);
    }
    return it;
}

struct widget *menu_add_separator(struct widget *menu)
{
    struct widget *it = widget_new(&menuitem_class, menu);
    if (it) {
        it->visible = 0;
        it->value = 1;
        widget_set_text(it, "");
    }
    return it;
}

/* ---- context menus ---- */

struct widget *popupmenu_new(struct widget *window)
{
    struct widget *m = widget_new(&menu_class, window);
    if (m)
        m->visible = 0;             /* only its dropdown is ever shown */
    return m;
}

void menu_popup(struct widget *menu, int x, int y)
{
    struct dropdown *d = (struct dropdown *)widget_new(&dropdown_class, NULL);
    if (!d)
        return;
    d->menu = menu;
    d->bar = NULL;
    d->hover = -1;
    d->w.app = menu->app;
    d->w.window = menu->window;
    struct size_hint h = { 0 };
    dropdown_measure(&d->w, &h);
    window_popup_open(menu->window, &d->w, x, y, h.pref_w, h.pref_h);
    widget_focus(&d->w);
}
