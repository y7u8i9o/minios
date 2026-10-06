/* Notifications in the panel (S5 of docs/plan/release-0.7.0.md,
 * docs/design/notifications.md).
 *
 * The panel is the display of notifyd. It connects to the "notify" socket,
 * mirrors the list of notifications and shows them in two places.
 *
 * - Pop-ups: the panel draws the visible notifications as cards, newest
 *   first and at most STACK_MAX of them. Each card has a layer surface of
 *   its own in the top layer, and the cards are stacked from the top right
 *   corner of the desktop area. A click on a card invokes its "default"
 *   action if it has one and otherwise dismisses the card. The close
 *   button dismisses the card, and the action buttons invoke their actions.
 * - History: the bell button left of the clock opens a popup with the
 *   newest notifications, a button that clears them and the do-not-disturb
 *   switch. The bell shows a dot while notifications have arrived that the
 *   user has not yet seen in the history.
 *
 * If notifyd is not running, the panel tries to connect once a second. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <gui/i18n.h>
#include "panel.h"
#include "notify-client.h"

#define STACK_MAX 4                     /* cards shown at once */
#define CARD_W 340
#define CARD_PAD 10
#define CARD_GAP 8
#define LINE_H 18
#define BODY_LINES 3
#define ACTION_H 30                     /* the row of action buttons */
#define ACTION_BTN_H 24
#define CLOSE_W 20
#define STACK_MARGIN 8
#define CRITICAL_BAR 0x00d9534f

#define HIST_W 340
#define HIST_PAD 6
#define HIST_HEAD_H 32
#define HIST_ROW_H 46
#define HIST_FOOT_H 32
#define HIST_ROWS 6

struct entry {
    uint32_t number;
    char app[64], summary[128], body[512], icon[64];
    unsigned urgency;
    time_t time;
    int nactions;
    char keys[3][32], labels[3][48];
    int popup;
    struct entry *next;                 /* newest first */
};

/* One pop-up card. It has a layer surface of its own in the top layer,
 * anchored to the top right corner of the desktop area. The surface is
 * opaque and rectangular. The panel stacks the cards through their top
 * margins. */
struct card {
    uint32_t number;
    struct entry *e;
    struct canvas cv;
    struct wire_proxy *layer;
    int configured;
    int y, h;                           /* position in the stack and height */
    int nbody;
    int bstart[BODY_LINES], blen[BODY_LINES];
    int nbuttons;
    int bx[3], bw[3], bidx[3];          /* action buttons: x, width, action index */
};

static struct wire_display *nd;
static struct wire_proxy *nd_registry, *nd_manager, *nd_display;
static struct entry *entries;
static int dnd, unseen;

static struct card *cards[STACK_MAX];
static int ncards;
static struct card *hover_card;
static int hover_button = -1;

static struct canvas hist;
static struct wire_proxy *hist_popup;
static int hist_open;

int notify_x(void)
{
    return clock_x() - NOTIFY_BTN_W - 4;
}

/* ---- the notification list ---- */

static struct entry *find(uint32_t number)
{
    for (struct entry *e = entries; e; e = e->next)
        if (e->number == number)
            return e;
    return NULL;
}

static int count_entries(void)
{
    int n = 0;
    for (struct entry *e = entries; e; e = e->next)
        n++;
    return n;
}

/* ---- the pop-up stack ---- */

/* Measure the card of e, which means the body lines that fit and the action
 * buttons. The text is measured with the font of the panel. */
static void layout_card(struct card *c, struct entry *e)
{
    struct painter p;
    canvas_painter(&p, &panel);
    c->e = e;
    c->number = e->number;
    int lines = painter_wrap(&p, e->body, CARD_W - 2 * CARD_PAD, c->bstart, c->blen, BODY_LINES);
    c->nbody = lines < BODY_LINES ? lines : BODY_LINES;
    c->nbuttons = 0;
    int x = CARD_PAD;
    for (int i = 0; i < e->nactions; i++) {
        if (strcmp(e->keys[i], "default") == 0)
            continue;
        int w = painter_text_width(&p, e->labels[i], -1) + 24;
        c->bx[c->nbuttons] = x;
        c->bw[c->nbuttons] = w;
        c->bidx[c->nbuttons] = i;
        c->nbuttons++;
        x += w + 6;
    }
    c->h = CARD_PAD + 2 * LINE_H + c->nbody * LINE_H + (c->nbuttons ? ACTION_H : 0) + CARD_PAD;
}

static void draw_card(struct card *c)
{
    if (!c->configured)
        return;
    const struct entry *e = c->e;
    struct painter pa, *p = &pa;
    canvas_painter(p, &c->cv);
    painter_fill(p, 0, 0, CARD_W, c->h, MENU_BG);
    painter_frame(p, 0, 0, CARD_W, c->h, MENU_BORDER);
    if (e->urgency == 2)
        painter_fill(p, 1, 1, 3, c->h - 2, CRITICAL_BAR);
    int y = CARD_PAD, hovered = c == hover_card;
    panel_label(p, CARD_PAD - 6, y, CARD_W - 2 * CARD_PAD - CLOSE_W, LINE_H, e->app[0] ? e->app : _("Notification"),
                MENU_TEXT_DIM, 0);
    const struct image *close = panel_icon("close", MENU_ICON);
    if (close) {
        int cx = CARD_W - CARD_PAD - CLOSE_W;
        if (hovered && hover_button == -2)
            painter_rounded(p, cx, y - 1, CLOSE_W, CLOSE_W, MENU_HOVER, 0xffffffffu);
        painter_image(p, cx + (CLOSE_W - image_lw(close)) / 2, y - 1 + (CLOSE_W - image_lh(close)) / 2, close);
    }
    y += LINE_H;
    panel_label(p, CARD_PAD - 6, y, CARD_W - 2 * CARD_PAD, LINE_H, e->summary, MENU_TEXT, 0);
    y += LINE_H;
    for (int i = 0; i < c->nbody; i++) {
        char line[512];
        int n = c->blen[i] < (int)sizeof line - 1 ? c->blen[i] : (int)sizeof line - 1;
        memcpy(line, e->body + c->bstart[i], (size_t)n);
        line[n] = '\0';
        panel_label(p, CARD_PAD - 6, y, CARD_W - 2 * CARD_PAD, LINE_H, line, MENU_TEXT, 0);
        y += LINE_H;
    }
    int by = y + (ACTION_H - ACTION_BTN_H) / 2;
    for (int i = 0; i < c->nbuttons; i++) {
        uint32_t bg = hovered && hover_button == i ? MENU_HOVER : METER_BG;
        painter_rounded(p, c->bx[i], by, c->bw[i], ACTION_BTN_H, bg, 0xffffffffu);
        panel_label(p, c->bx[i], by, c->bw[i], ACTION_BTN_H, e->labels[c->bidx[i]], MENU_TEXT, 1);
    }
    canvas_commit(&c->cv);
}

static void card_free(struct card *c)
{
    if (hover_card == c)
        hover_card = NULL;
    if (c->layer)
        layer_surface_destroy(c->layer);
    if (c->cv.surface) {
        surface_destroy(c->cv.surface);
        canvas_release_buffer(&c->cv);
    }
    free(c);
}

static void on_card_configure(void *user, struct wire_proxy *l, uint32_t serial, int32_t w, int32_t h)
{
    struct card *c = user;
    layer_surface_ack_configure(l, serial);
    if (c->cv.lw != CARD_W || c->cv.lh != c->h || c->cv.scale != output_scale)
        if (canvas_resize(&c->cv, CARD_W, c->h) < 0)
            return;
    c->configured = 1;
    draw_card(c);
}

static void on_card_closed(void *user, struct wire_proxy *l)
{
}

static const struct layer_surface_listener card_events = { on_card_configure, on_card_closed };

/* Request the size and position of c in the stack. The card is drawn when
 * the compositor answers with a configure event. */
static void place_card(struct card *c)
{
    layer_surface_set_margin(c->layer, STACK_MARGIN + c->y, STACK_MARGIN, STACK_MARGIN, STACK_MARGIN);
    layer_surface_set_size(c->layer, CARD_W, c->h);
    surface_commit(c->cv.surface);
}

/* Update the cards to match the visible notifications, newest first. The
 * function reuses the card of a notification that is already shown and
 * creates cards for the other notifications. It removes the cards of
 * notifications that are no longer visible. */
static void update_stack(void)
{
    struct card *next[STACK_MAX];
    int n = 0, y = 0;
    for (struct entry *e = entries; e && n < STACK_MAX; e = e->next) {
        if (!e->popup)
            continue;
        struct card *c = NULL;
        for (int i = 0; i < ncards; i++)
            if (cards[i] && cards[i]->number == e->number) {
                c = cards[i];
                cards[i] = NULL;
                break;
            }
        int old_y = c ? c->y : -1, old_h = c ? c->h : -1;
        if (!c) {
            c = calloc(1, sizeof *c);
            if (!c || canvas_create(&c->cv, CARD_W, 1) < 0) {
                if (c)
                    card_free(c);
                continue;
            }
            c->layer = shell_get_layer_surface(shell, c->cv.surface, 2, "notification");
            layer_surface_add_listener(c->layer, &card_events, c);
            layer_surface_set_anchor(c->layer, 1 | 8);         /* top and right */
        }
        layout_card(c, e);
        c->y = y;
        y += c->h + CARD_GAP;
        if (c->y != old_y || c->h != old_h)
            place_card(c);
        else
            draw_card(c);
        next[n++] = c;
    }
    for (int i = 0; i < ncards; i++)
        if (cards[i])
            card_free(cards[i]);
    memcpy(cards, next, sizeof next[0] * (size_t)n);
    ncards = n;
    wire_display_flush(display);
}

int notify_owns(const struct wire_proxy *surface)
{
    if (!surface)
        return 0;
    if (hist.surface && surface == hist.surface)
        return 1;
    for (int i = 0; i < ncards; i++)
        if (cards[i]->cv.surface == surface)
            return 1;
    return 0;
}

static struct card *card_of(const struct wire_proxy *surface)
{
    for (int i = 0; i < ncards; i++)
        if (cards[i]->cv.surface == surface)
            return cards[i];
    return NULL;
}

/* Return the part of card c at (x, y). The result is an action button
 * (0 and up), the close button (-2) or the card itself (-1). */
static int card_hit(const struct card *c, int x, int y)
{
    int cx = CARD_W - CARD_PAD - CLOSE_W;
    if (x >= cx && x < cx + CLOSE_W && y >= CARD_PAD - 1 && y < CARD_PAD - 1 + CLOSE_W)
        return -2;
    int by = CARD_PAD + 2 * LINE_H + c->nbody * LINE_H + (ACTION_H - ACTION_BTN_H) / 2;
    for (int b = 0; b < c->nbuttons; b++)
        if (x >= c->bx[b] && x < c->bx[b] + c->bw[b] && y >= by && y < by + ACTION_BTN_H)
            return b;
    return -1;
}

/* ---- the history popup ---- */

static int hist_height(void)
{
    int n = count_entries();
    int rows = n > HIST_ROWS ? HIST_ROWS : n;
    return 2 * HIST_PAD + HIST_HEAD_H + (rows ? rows * HIST_ROW_H : HIST_ROW_H) + HIST_FOOT_H;
}

static void draw_history(void)
{
    if (!hist_open || !hist.surface)
        return;
    struct painter p;
    canvas_painter(&p, &hist);
    painter_fill(&p, 0, 0, hist.lw, hist.lh, MENU_BG);
    painter_frame(&p, 0, 0, hist.lw, hist.lh, MENU_BORDER);
    int y = HIST_PAD, w = hist.lw - 2 * HIST_PAD;
    panel_label(&p, HIST_PAD, y, w, HIST_HEAD_H, _("Notifications"), MENU_TEXT, 0);
    if (entries) {
        const char *clear = _("Clear all");
        int tw = painter_text_width(&p, clear, -1) + 12;
        panel_label(&p, HIST_PAD + w - tw, y, tw, HIST_HEAD_H, clear, ACCENT, 1);
    }
    y += HIST_HEAD_H;
    const struct image *close = panel_icon("close", MENU_ICON);
    int rows = 0;
    for (struct entry *e = entries; e && rows < HIST_ROWS; e = e->next, rows++) {
        painter_line(&p, HIST_PAD, y, HIST_PAD + w, y, MENU_BORDER);
        char when[16];
        struct tm tm;
        localtime_r(&e->time, &tm);
        strftime(when, sizeof when, "%H:%M", &tm);
        int tw = painter_text_width(&p, when, -1) + 12;
        panel_label(&p, HIST_PAD, y + 4, w - tw - CLOSE_W, LINE_H, e->summary, MENU_TEXT, 0);
        panel_label(&p, HIST_PAD + w - tw - CLOSE_W, y + 4, tw, LINE_H, when, MENU_TEXT_DIM, 1);
        /* Show the first line of the body, or the application name if the
         * body is empty. */
        int start[1], len[1];
        char line[512];
        if (e->body[0] && painter_wrap(&p, e->body, w - CLOSE_W - 12, start, len, 1) > 0) {
            int n = len[0] < (int)sizeof line - 1 ? len[0] : (int)sizeof line - 1;
            memcpy(line, e->body + start[0], (size_t)n);
            line[n] = '\0';
        } else {
            strlcpy(line, e->app, sizeof line);
        }
        panel_label(&p, HIST_PAD, y + 4 + LINE_H, w - CLOSE_W, LINE_H, line, MENU_TEXT_DIM, 0);
        if (close)
            painter_image(&p, HIST_PAD + w - CLOSE_W + (CLOSE_W - image_lw(close)) / 2,
                          y + (HIST_ROW_H - image_lh(close)) / 2, close);
        y += HIST_ROW_H;
    }
    if (!rows) {
        panel_label(&p, HIST_PAD, y, w, HIST_ROW_H, _("No notifications"), MENU_TEXT_DIM, 1);
        y += HIST_ROW_H;
    }
    painter_line(&p, HIST_PAD, y, HIST_PAD + w, y, MENU_BORDER);
    panel_label(&p, HIST_PAD, y, w - 50, HIST_FOOT_H, _("Do not disturb"), MENU_TEXT, 0);
    /* The switch is a pill with a knob. It uses the accent colour while on. */
    int sx = HIST_PAD + w - 40, sy = y + (HIST_FOOT_H - 18) / 2;
    painter_rounded(&p, sx, sy, 36, 18, dnd ? ACCENT : METER_BG, 0xffffffffu);
    painter_rounded(&p, sx + (dnd ? 20 : 2), sy + 2, 14, 14, 0x00ffffff, 0xffffffffu);
    canvas_commit(&hist);
}

static void hist_teardown(void)
{
    if (hist_popup) {
        popup_destroy(hist_popup);
        hist_popup = NULL;
    }
    if (hist.surface) {
        surface_destroy(hist.surface);
        canvas_release_buffer(&hist);
        memset(&hist, 0, sizeof hist);
        hist.fd = -1;
    }
    if (hist_open)
        log_line("notifications history closed");
    hist_open = 0;
}

static void on_hist_configure(void *user, struct wire_proxy *p, uint32_t serial, int32_t x, int32_t y, int32_t w,
                              int32_t h)
{
    popup_ack_configure(p, serial);
    popup_grab(hist_popup, seat, press_serial);
    draw_history();
    log_line("notifications history opened, %d entries", count_entries());
}

static void on_hist_done(void *user, struct wire_proxy *p)
{
    hist_teardown();
    draw_panel();
}

static const struct popup_listener hist_events = { on_hist_configure, on_hist_done };

int notify_history_is_open(void)
{
    return hist_open;
}

void notify_history_toggle(void)
{
    if (hist_open) {
        hist_teardown();
        draw_panel();
        return;
    }
    int h = hist_height();
    if (canvas_create(&hist, HIST_W, h) < 0) {
        hist_teardown();
        return;
    }
    struct wire_proxy *pos = shell_create_positioner(shell);
    positioner_set_size(pos, HIST_W, h);
    panel_place_popup(pos, notify_x(), NOTIFY_BTN_W, 1);
    hist_popup = shell_get_popup(shell, hist.surface, panel.surface, pos);
    popup_add_listener(hist_popup, &hist_events, NULL);
    positioner_destroy(pos);
    hist_open = 1;
    unseen = 0;
    draw_panel();
    wire_display_flush(display);
}

/* Update the history popup after a change while it is open. The height of
 * the popup depends on the number of entries, so the popup opens again at
 * the new size. */
static void refresh_history(void)
{
    if (!hist_open)
        return;
    if (hist.lh != hist_height()) {
        hist_teardown();
        notify_history_toggle();
    } else {
        draw_history();
    }
}

/* ---- pointer input ---- */

void notify_pointer_motion(const struct wire_proxy *surface, int x, int y)
{
    struct card *c = card_of(surface);
    int button = c ? card_hit(c, x, y) : -1;
    if (c != hover_card || button != hover_button) {
        struct card *old = hover_card;
        hover_card = c;
        hover_button = button;
        if (old && old != c)
            draw_card(old);
        if (c)
            draw_card(c);
    }
}

void notify_pointer_button(const struct wire_proxy *surface, uint32_t button, uint32_t state, int x, int y)
{
    if (button != 1 || state != 1 || !nd_display)
        return;
    struct card *c = card_of(surface);
    if (c) {
        struct entry *e = c->e;
        int part = card_hit(c, x, y);
        if (part >= 0) {
            log_line("notification %u action %s", e->number, e->keys[c->bidx[part]]);
            notify_display_invoke(nd_display, e->number, e->keys[c->bidx[part]]);
        } else {
            int has_default = 0;
            for (int a = 0; a < e->nactions; a++)
                has_default |= strcmp(e->keys[a], "default") == 0;
            if (part == -1 && has_default) {
                log_line("notification %u action default", e->number);
                notify_display_invoke(nd_display, e->number, "default");
            } else {
                log_line("notification %u dismissed", e->number);
                notify_display_dismiss(nd_display, e->number);
            }
        }
        wire_display_flush(nd);
        return;
    }
    if (!hist_open || surface != hist.surface)
        return;
    int w = hist.lw - 2 * HIST_PAD;
    int rows = 0;
    for (struct entry *e = entries; e && rows < HIST_ROWS; e = e->next)
        rows++;
    int foot = HIST_PAD + HIST_HEAD_H + (rows ? rows * HIST_ROW_H : HIST_ROW_H);
    if (y >= HIST_PAD && y < HIST_PAD + HIST_HEAD_H && x >= HIST_PAD + w / 2 && entries) {
        log_line("notifications cleared");
        notify_display_clear(nd_display);
    } else if (y >= foot && y < foot + HIST_FOOT_H) {
        log_line("notifications do not disturb %s", dnd ? "off" : "on");
        notify_display_set_dnd(nd_display, !dnd);
    } else if (y >= HIST_PAD + HIST_HEAD_H && y < foot && x >= HIST_PAD + w - CLOSE_W) {
        int row = (y - HIST_PAD - HIST_HEAD_H) / HIST_ROW_H, k = 0;
        for (struct entry *e = entries; e; e = e->next, k++)
            if (k == row) {
                log_line("notification %u removed", e->number);
                notify_display_remove(nd_display, e->number);
                break;
            }
    }
    wire_display_flush(nd);
}

/* ---- the button ---- */

void notify_draw_button(struct painter *p, int hovered)
{
    int x = notify_x(), h = panel.lh;
    if (hist_open || hovered)
        painter_rounded(p, x, BUTTON_Y, NOTIFY_BTN_W, h - 2 * BUTTON_Y, hist_open ? BUTTON_OPEN : BUTTON_HOVER,
                        0xffffffffu);
    const struct image *bell = panel_icon(dnd ? "notifications-off" : "notifications", PANEL_TEXT);
    if (bell)
        painter_image(p, x + (NOTIFY_BTN_W - image_lw(bell)) / 2, (h - image_lh(bell)) / 2, bell);
    if (unseen && !dnd)
        painter_rounded(p, x + NOTIFY_BTN_W - 10, BUTTON_Y + 3, 6, 6, ACCENT, 0xffffffffu);
}

/* ---- the connection to notifyd ---- */

static void parse_actions(struct entry *e, const char *actions)
{
    e->nactions = 0;
    const char *at = actions;
    while (*at && e->nactions < 3) {
        const char *nl = strchr(at, '\n');
        if (!nl)
            break;
        const char *nl2 = strchr(nl + 1, '\n');
        if (!nl2)
            break;
        size_t kl = (size_t)(nl - at), ll = (size_t)(nl2 - nl - 1);
        if (kl >= sizeof e->keys[0])
            kl = sizeof e->keys[0] - 1;
        if (ll >= sizeof e->labels[0])
            ll = sizeof e->labels[0] - 1;
        memcpy(e->keys[e->nactions], at, kl);
        e->keys[e->nactions][kl] = '\0';
        memcpy(e->labels[e->nactions], nl + 1, ll);
        e->labels[e->nactions][ll] = '\0';
        e->nactions++;
        at = nl2 + 1;
    }
}

static void on_notification(void *user, struct wire_proxy *d, uint32_t number, const char *app, const char *summary,
                            const char *body, const char *icon, uint32_t urgency, uint32_t time,
                            const char *actions)
{
    struct entry *e = find(number);
    if (!e) {
        e = calloc(1, sizeof *e);
        if (!e)
            return;
        e->number = number;
        e->next = entries;
        entries = e;
        unseen = !hist_open;
    }
    strlcpy(e->app, app, sizeof e->app);
    strlcpy(e->summary, summary, sizeof e->summary);
    strlcpy(e->body, body, sizeof e->body);
    strlcpy(e->icon, icon, sizeof e->icon);
    e->urgency = urgency;
    e->time = (time_t)time;
    parse_actions(e, actions);
    if (e->popup)
        update_stack();
    refresh_history();
    draw_panel();
}

static void on_popup(void *user, struct wire_proxy *d, uint32_t number, uint32_t visible)
{
    struct entry *e = find(number);
    if (!e || e->popup == (int)visible)
        return;
    e->popup = (int)visible;
    log_line("notification %u popup %s '%s'", number, visible ? "shown" : "hidden", e->summary);
    update_stack();
}

static void on_removed(void *user, struct wire_proxy *d, uint32_t number)
{
    for (struct entry **p = &entries; *p; p = &(*p)->next)
        if ((*p)->number == number) {
            struct entry *e = *p;
            *p = e->next;
            int was_popup = e->popup;
            free(e);
            if (was_popup)
                update_stack();
            break;
        }
    if (!entries)
        unseen = 0;
    refresh_history();
    draw_panel();
}

static void on_dnd(void *user, struct wire_proxy *d, uint32_t on)
{
    dnd = (int)on;
    refresh_history();
    draw_panel();
}

static const struct notify_display_listener display_events = { on_notification, on_popup, on_removed, on_dnd };

static void on_global(void *user, struct wire_proxy *registry, uint32_t name, const char *iface, uint32_t version)
{
    if (strcmp(iface, "notify_manager") == 0 && !nd_manager)
        nd_manager = registry_bind(registry, name, iface, version, &notify_manager_interface, 1);
}

static void on_global_remove(void *user, struct wire_proxy *registry, uint32_t name)
{
}

static const struct registry_listener registry_events = { on_global, on_global_remove };

static void disconnect(void)
{
    if (nd)
        wire_display_disconnect(nd);
    nd = NULL;
    nd_registry = nd_manager = nd_display = NULL;
    while (entries) {
        struct entry *e = entries;
        entries = e->next;
        free(e);
    }
    unseen = 0;
    update_stack();
    refresh_history();
}

void notify_connect(void)
{
    if (nd)
        return;
    nd = wire_display_connect("notify");
    if (!nd)
        return;
    nd_registry = display_get_registry(wire_display_proxy(nd));
    registry_add_listener(nd_registry, &registry_events, NULL);
    if (wire_display_roundtrip(nd) < 0 || !nd_manager) {
        disconnect();
        return;
    }
    nd_display = notify_manager_get_display(nd_manager);
    notify_display_add_listener(nd_display, &display_events, NULL);
    wire_display_flush(nd);
    log_line("notifications connected");
}

int notify_fd(void)
{
    return nd ? wire_display_fd(nd) : -1;
}

void notify_dispatch(int revents)
{
    if (!nd)
        return;
    if (wire_display_dispatch(nd) < 0) {
        log_line("notifications disconnected");
        disconnect();
        draw_panel();
        return;
    }
    wire_display_flush(nd);
}
