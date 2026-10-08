/* Menu bar with drop down menus shown as window popups. */
#include <gui/app.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../intl.h"

/* The horizontal padding of titles and items. */
static int menu_pad(const struct widget *w)
{
    return theme_scale_px(widget_theme(w), 10);
}

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

/* A menu item: plain, a check item or a radio item, and optionally the
 * item that opens a submenu. A separator has the value 1. */
enum { ITEM_PLAIN, ITEM_CHECK, ITEM_RADIO };

struct menuitem {
    struct widget w;
    int kind, checked;
    struct widget *submenu;
};

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
    h->pref_w = widget_text_width(w, NULL, caption, -1) + 2 * menu_pad(w) + 24;
    if (accel[0])
        h->pref_w += widget_text_width(w, NULL, accel, -1) + 24;
    if (((struct menuitem *)w)->submenu)
        h->pref_w += theme_px(t, TM_ICON);
    h->pref_h = h->min_h = theme_px(t, TM_CONTROL_H);
}

const struct widget_class menuitem_class = { "menuitem", sizeof(struct menuitem), item_measure, NULL, NULL, NULL, NULL };
const struct widget_class menu_class = { "menu", sizeof(struct widget), NULL, NULL, NULL, NULL, NULL };

/* ---- the drop down popup ---- */

static const struct widget_class dropdown_class;

/* A column shows the items of one menu. The drop down shows the column of
 * its menu and, while a submenu is open, a second column for the submenu
 * at the height of its item. level is the column that the keys move in. */
struct column {
    struct widget *menu;
    int x, y, w, h;
    int hover;
    int *accel_w;               /* the widths of the accelerator labels */
};

struct dropdown {
    struct widget w;
    struct widget *bar;
    struct column col[2];
    int ncols, level;
};

void widget_measure(struct widget *w);

static int count_items(struct widget *menu)
{
    int n = 0;
    for (struct widget *it = menu->first; it; it = it->next)
        n++;
    return n;
}

static struct widget *item_n(struct widget *menu, int n)
{
    struct widget *it = menu->first;
    while (it && n-- > 0)
        it = it->next;
    return it;
}

/* Measures the items of a column and its size. */
static void measure_column(struct dropdown *d, struct column *c)
{
    int mw = 80, mh = 2, n = 0;
    free(c->accel_w);
    c->accel_w = calloc((size_t)(count_items(c->menu) + 1), sizeof *c->accel_w);
    for (struct widget *it = c->menu->first; it; it = it->next, n++) {
        char accel[48];
        accel_label(it, accel, sizeof accel);
        if (c->accel_w && accel[0])
            c->accel_w[n] = widget_text_width(&d->w, NULL, accel, -1);
        widget_measure(it);
        if (it->measured.pref_w > mw) mw = it->measured.pref_w;
        mh += it->measured.pref_h;
    }
    c->w = mw;
    c->h = mh;
}

static void dropdown_measure(struct widget *w, struct size_hint *h)
{
    struct dropdown *d = (struct dropdown *)w;
    int width = 0, height = 0;
    for (int i = 0; i < d->ncols; i++) {
        measure_column(d, &d->col[i]);
        if (i)
            d->col[i].x = d->col[0].w;
        if (d->col[i].x + d->col[i].w > width) width = d->col[i].x + d->col[i].w;
        if (d->col[i].y + d->col[i].h > height) height = d->col[i].y + d->col[i].h;
    }
    h->pref_w = width;
    h->pref_h = height;
}

/* The top of item n of a column, inside the column. */
static int item_top(struct column *c, int n)
{
    int y = 1;
    for (struct widget *it = c->menu->first; it && n-- > 0; it = it->next)
        y += it->measured.pref_h;
    return y;
}

static void paint_column(struct dropdown *d, struct painter *p, struct column *c)
{
    const struct theme *t = p->theme;
    struct widget *w = &d->w;
    painter_push(p, c->x, c->y, c->w, c->h);
    painter_fill(p, 0, 0, c->w, c->h, t->color[TC_FIELD]);
    painter_frame(p, 0, 0, c->w, c->h, t->color[TC_BORDER]);
    int y = 1, i = 0, mark = theme_px(t, TM_ICON);
    for (struct widget *it = c->menu->first; it; it = it->next, i++) {
        struct menuitem *mi = (struct menuitem *)it;
        int ih = it->measured.pref_h;
        uint32_t fg = t->color[it->enabled ? TC_TEXT : TC_TEXT_DISABLED];
        if (it->value == 1) {
            painter_line(p, 4, y + ih / 2, c->w - 5, y + ih / 2, t->color[TC_BORDER]);
            y += ih;
            continue;
        }
        if (i == c->hover && it->enabled)
            painter_rounded(p, 3, y + 1, c->w - 6, ih - 2, t->color[TC_HIGHLIGHT], PAINTER_NONE);
        int x = menu_pad(w);
        if (mi->kind == ITEM_CHECK && mi->checked)
            painter_check(p, 4, y + (ih - mark) / 2, mark, fg);
        else if (mi->kind == ITEM_RADIO && mi->checked)
            painter_disc(p, 4 + mark / 3, y + (ih - mark / 3) / 2, mark / 3, fg);
        else if (it->icon)
            painter_icon(p, 4, y + (ih - image_lh(it->icon)) / 2, it->icon, !it->enabled);
        if (mi->kind != ITEM_PLAIN || it->icon)
            x = 24;
        painter_mnemonic_text(p, x, y + (ih - painter_text_height(p)) / 2, widget_text(it), fg);
        char accel[48];
        accel_label(it, accel, sizeof accel);
        if (accel[0] && c->accel_w)
            painter_text(p, c->w - menu_pad(w) - c->accel_w[i], y + (ih - painter_text_height(p)) / 2, accel,
                         t->color[TC_TEXT_DISABLED]);
        if (mi->submenu)
            painter_chevron(p, c->w - mark - 4, y + (ih - mark) / 2, mark, PAINTER_RIGHT, fg);
        y += ih;
    }
    painter_pop(p);
}

static void dropdown_paint(struct widget *w, struct painter *p)
{
    struct dropdown *d = (struct dropdown *)w;
    painter_fill(p, 0, 0, w->w, w->h, p->theme->color[TC_WINDOW]);
    for (int i = 0; i < d->ncols; i++)
        paint_column(d, p, &d->col[i]);
}

/* The item of a column at the local position (px, py) of the drop down,
 * or -1 for a separator or outside the column. */
static int item_at(struct column *c, int px, int py)
{
    if (px < c->x || px >= c->x + c->w)
        return -1;
    int y = c->y + 1, i = 0;
    for (struct widget *it = c->menu->first; it; it = it->next, i++) {
        if (py >= y && py < y + it->measured.pref_h)
            return it->value == 1 ? -1 : i;
        y += it->measured.pref_h;
    }
    return -1;
}

/* The next item that can be chosen, searched from n in the direction
 * step. The search skips separators and disabled items. The function
 * returns n when no such item exists. */
static int step_item(struct widget *menu, int n, int step)
{
    int count = count_items(menu);
    for (int k = 1; k <= count; k++) {
        int i = ((n + step * k) % count + count) % count;
        if (n < 0 && step < 0)
            i = ((count - k) % count + count) % count;
        struct widget *it = item_n(menu, i);
        if (it && it->value != 1 && it->enabled)
            return i;
    }
    return n;
}

static struct dropdown *dropdown_new(struct widget *menu, struct widget *bar, struct widget *window)
{
    struct dropdown *d = (struct dropdown *)widget_new(&dropdown_class, NULL);
    if (!d)
        return NULL;
    d->col[0].menu = menu;
    d->col[0].hover = -1;
    d->ncols = 1;
    d->bar = bar;
    d->w.app = window->app;
    d->w.window = window;
    return d;
}

/* Opens the drop down at window coordinates. The popup is created again
 * when a submenu opens or closes, because its size changes. */
static void dropdown_show(struct dropdown *d, int x, int y)
{
    struct size_hint h = { 0 };
    dropdown_measure(&d->w, &h);
    window_popup_open(d->w.window, &d->w, x, y, h.pref_w, h.pref_h);
    widget_focus(&d->w);
}

/* Shows the drop down again with the submenu of item n of the first
 * column, or without a submenu for n < 0. */
static void set_submenu(struct dropdown *d, int n)
{
    struct menuitem *mi = n >= 0 ? (struct menuitem *)item_n(d->col[0].menu, n) : NULL;
    struct widget *sub = mi && mi->w.enabled ? mi->submenu : NULL;
    if ((d->ncols == 2 ? d->col[1].menu : NULL) == sub)
        return;
    int ax, ay;
    widget_abs(&d->w, &ax, &ay);
    struct dropdown *e = dropdown_new(d->col[0].menu, d->bar, d->w.window);
    if (!e)
        return;
    e->col[0].hover = d->col[0].hover;
    if (sub) {
        e->col[1].menu = sub;
        e->col[1].hover = -1;
        e->col[1].y = item_top(&d->col[0], n) - 1;
        e->ncols = 2;
    }
    window_popup_close(d->w.window);
    dropdown_show(e, ax, ay);
}

/* Chooses an item: closes the menu, updates a check or radio item and
 * emits "clicked". */
static void activate(struct dropdown *d, struct widget *it)
{
    if (!it || it->value == 1 || !it->enabled)
        return;
    struct menuitem *mi = (struct menuitem *)it;
    if (mi->submenu)
        return;
    struct widget *bar = d->bar;
    window_popup_close(d->w.window);
    if (bar) {
        bar->value = -1;
        widget_invalidate(bar);
    }
    if (mi->kind == ITEM_CHECK)
        menuitem_set_check(it, !mi->checked);
    else if (mi->kind == ITEM_RADIO)
        menuitem_set_radio(it, 1);
    struct sig_click c = { 1, 0, 0 };
    widget_emit(it, "clicked", &c);
}

static int dropdown_event(struct widget *w, struct event *e)
{
    struct dropdown *d = (struct dropdown *)w;
    struct column *c0 = &d->col[0], *c1 = d->ncols > 1 ? &d->col[1] : NULL;
    switch (e->type) {
    case EV_MOUSE_MOVE: {
        if (c1 && e->x >= c1->x) {
            int h = item_at(c1, e->x, e->y);
            if (h != c1->hover) {
                c1->hover = h;
                d->level = 1;
                widget_invalidate(w);
            }
            return 1;
        }
        int h = item_at(c0, e->x, e->y);
        if (h != c0->hover) {
            c0->hover = h;
            d->level = 0;
            widget_invalidate(w);
            set_submenu(d, h);
        }
        return 1;
    }
    case EV_MOUSE_DOWN:
        if (c1 && e->x >= c1->x) {
            int n = item_at(c1, e->x, e->y);
            activate(d, n >= 0 ? item_n(c1->menu, n) : NULL);
        } else {
            int n = item_at(c0, e->x, e->y);
            if (n >= 0 && ((struct menuitem *)item_n(c0->menu, n))->submenu)
                set_submenu(d, n);
            else
                activate(d, n >= 0 ? item_n(c0->menu, n) : NULL);
        }
        return 1;
    case EV_KEY_DOWN: {
        struct column *c = d->level && c1 ? c1 : c0;
        if (e->code == KEY_DOWN || e->code == KEY_UP) {
            int next = step_item(c->menu, c->hover, e->code == KEY_DOWN ? 1 : -1);
            if (next != c->hover) {
                c->hover = next;
                widget_invalidate(w);
            }
            return 1;
        }
        if (e->ch == '\n') {
            struct widget *it = c->hover >= 0 ? item_n(c->menu, c->hover) : NULL;
            if (c == c0 && it && ((struct menuitem *)it)->submenu)
                set_submenu(d, c->hover);
            else
                activate(d, it);
            return 1;
        }
        if (e->code == KEY_RIGHT && c == c0 && c0->hover >= 0 &&
            ((struct menuitem *)item_n(c0->menu, c0->hover))->submenu) {
            /* set_submenu frees the drop down. The window pointer is read
             * before the call. The new drop down has the focus. */
            struct window_state *ws = window_state_of(w->window);
            set_submenu(d, c0->hover);
            struct dropdown *nd = ws->popup ? (struct dropdown *)ws->popup : NULL;
            if (nd && nd->ncols == 2) {
                nd->level = 1;
                nd->col[1].hover = step_item(nd->col[1].menu, -1, 1);
            }
            return 1;
        }
        if (e->code == KEY_LEFT && c == c1) {
            set_submenu(d, -1);
            return 1;
        }
        if ((e->code == KEY_LEFT || e->code == KEY_RIGHT) && d->bar) {
            struct widget *bar = d->bar;
            int count = count_items(bar);
            menubar_open(bar, (bar->value + (e->code == KEY_RIGHT ? 1 : count - 1)) % count);
            return 1;
        }
        return 1;
    }
    default:
        return 0;
    }
}

static void dropdown_destroy(struct widget *w)
{
    struct dropdown *d = (struct dropdown *)w;
    free(d->col[0].accel_w);
    free(d->col[1].accel_w);
}

static const struct widget_class dropdown_class = { "dropdown", sizeof(struct dropdown), dropdown_measure, NULL,
                                                    dropdown_paint, dropdown_event, dropdown_destroy };

/* ---- menu bar ---- */

/* The menu bar measures the widths of its titles once per measurement. */
struct menubar {
    struct widget w;
    int *title_w, ntitles;
    int hot;                    /* the title under the pointer, or -1 */
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
        widths[n] = widget_text_width(w, NULL, caption, -1) + 2 * menu_pad(w);
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
        if (i == w->value || i == ((struct menubar *)w)->hot)
            painter_rounded(p, x + 1, 2, tw - 2, w->h - 4, t->color[i == w->value ? TC_HIGHLIGHT : TC_BUTTON_HOVER],
                            PAINTER_NONE);
        painter_mnemonic_text(p, x + menu_pad(w), 4, widget_text(m), t->color[TC_TEXT]);
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
    struct dropdown *d = dropdown_new(m, bar, bar->window);
    if (!d)
        return;
    int ax, ay;
    widget_abs(bar, &ax, &ay);
    dropdown_show(d, ax + x, ay + bar->h);
}

static int menubar_event(struct widget *w, struct event *e)
{
    struct menubar *mb = (struct menubar *)w;
    if (e->type == EV_MOUSE_MOVE || e->type == EV_LEAVE) {
        int hot = -1, x = 0, i = 0;
        for (struct widget *m = w->first; m && e->type == EV_MOUSE_MOVE; m = m->next, i++) {
            int tw = title_width(w, m);
            if (e->x >= x && e->x < x + tw)
                hot = i;
            x += tw;
        }
        if (hot != mb->hot) {
            mb->hot = hot;
            widget_invalidate(w);
        }
        return 0;
    }
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
    if (w) {
        w->value = -1;
        ((struct menubar *)w)->hot = -1;
    }
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
    struct dropdown *d = dropdown_new(menu, NULL, menu->window);
    if (d)
        dropdown_show(d, x, y);
}

struct widget *menu_add_submenu(struct widget *menu, const char *text)
{
    struct widget *it = menu_add(menu, text, NULL);
    if (!it)
        return NULL;
    struct widget *sub = widget_new(&menu_class, it);
    if (sub)
        sub->visible = 0;
    ((struct menuitem *)it)->submenu = sub;
    return sub;
}

void menuitem_set_check(struct widget *it, int checked)
{
    struct menuitem *mi = (struct menuitem *)it;
    mi->kind = ITEM_CHECK;
    mi->checked = checked != 0;
    widget_invalidate(it);
}

void menuitem_set_radio(struct widget *it, int checked)
{
    struct menuitem *mi = (struct menuitem *)it;
    mi->kind = ITEM_RADIO;
    mi->checked = checked != 0;
    /* The radio items of one menu exclude each other. */
    if (mi->checked && it->parent)
        for (struct widget *o = it->parent->first; o; o = o->next)
            if (o != it && o->cls == &menuitem_class && ((struct menuitem *)o)->kind == ITEM_RADIO)
                ((struct menuitem *)o)->checked = 0;
    widget_invalidate(it);
}

int menuitem_checked(const struct widget *it)
{
    return ((const struct menuitem *)it)->checked;
}
