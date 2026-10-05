/* Text input and X12's small built-in Unicode composer. Physical keys
 * remain on the keyboard interface; committed text and preedit state use
 * this protocol so clients do not have to infer text from scancodes.
 * The input method daemon composes through the same contexts
 * (inputmethod.c), and a key that a composition uses does not reach the
 * client as a key event. */
#include <stdlib.h>
#include <string.h>
#include <gui/keymap.h>
#include <gui/utf8.h>
#include "comp.h"

struct text_context {
    struct wire_resource *res;
    struct client *client;
    struct csurface *active, *pending;
    int pending_action;                 /* -1 unchanged, 0 disable, 1 enable */
    int entered;
    uint32_t serial, hints, purpose;
    char surrounding[1024];
    uint32_t cursor, anchor;
    int composing, nhex;
    char hex[7];
    int dead;                           /* the dead key that waits for its base, or 0 */
    int rect[4], pending_rect[4];       /* the caret in surface coordinates: x, y, width, height */
    int has_rect, has_pending_rect;
};

static void end_preedit(struct text_context *t)
{
    if (!t || !t->composing)
        return;
    t->composing = 0;
    t->nhex = 0;
    t->hex[0] = '\0';
    text_input_send_preedit_string(t->res, "", 0, 0);
    text_input_send_done(t->res, t->serial);
}

static void send_preedit(struct text_context *t)
{
    char shown[9] = "u";
    memcpy(shown + 1, t->hex, (size_t)t->nhex + 1);
    text_input_send_preedit_string(t->res, shown, 0, t->nhex + 1);
    text_input_send_done(t->res, t->serial);
}

static void update_anchor(struct text_context *t);

static struct text_context *context_of(struct client *cl)
{
    return cl && cl->text_input ? cl->text_input->data : NULL;
}

static void leave(struct text_context *t)
{
    if (t && t->entered) {
        end_preedit(t);
        /* A preedit of the input method daemon ends with the focus. */
        text_input_send_preedit_string(t->res, "", 0, 0);
        text_input_send_done(t->res, t->serial);
        text_input_send_leave(t->res, t->active->res);
        t->entered = 0;
        im_update();
    }
}

static void enter(struct text_context *t)
{
    if (t && t->active && seat_keyboard_focus() == t->active && !t->entered) {
        text_input_send_enter(t->res, t->active->res);
        t->entered = 1;
        update_anchor(t);
        im_update();
    }
}

static void h_enable(struct wire_client *c, struct wire_resource *self, struct wire_resource *surface)
{
    struct text_context *t = self->data;
    struct csurface *s = surface ? surface->data : NULL;
    if (!s || s->client != t->client) {
        wire_client_post_error(c, self, 40, "text input surface belongs to another client");
        return;
    }
    t->pending = s;
    t->pending_action = 1;
}

static void h_disable(struct wire_client *c, struct wire_resource *self)
{
    struct text_context *t = self->data;
    t->pending = NULL;
    t->pending_action = 0;
}

static int boundary(const char *s, uint32_t at)
{
    size_t n = strlen(s);
    return at <= n && (at == 0 || at == n || ((unsigned char)s[at] & 0xc0) != 0x80);
}

static void h_surrounding(struct wire_client *c, struct wire_resource *self, const char *text,
                          uint32_t cursor, uint32_t anchor)
{
    struct text_context *t = self->data;
    if (!boundary(text, cursor) || !boundary(text, anchor)) {
        wire_client_post_error(c, self, 41, "surrounding text offsets are not UTF-8 boundaries");
        return;
    }
    strlcpy(t->surrounding, text, sizeof t->surrounding);
    size_t retained = strlen(t->surrounding);
    t->cursor = cursor <= retained ? cursor : (uint32_t)retained;
    t->anchor = anchor <= retained ? anchor : (uint32_t)retained;
}

static void h_content(struct wire_client *c, struct wire_resource *self, uint32_t hints, uint32_t purpose)
{
    struct text_context *t = self->data;
    t->hints = hints;
    t->purpose = purpose;
}

static void h_cursor_rectangle(struct wire_client *c, struct wire_resource *self, int32_t x, int32_t y,
                               int32_t width, int32_t height)
{
    struct text_context *t = self->data;
    t->pending_rect[0] = x;
    t->pending_rect[1] = y;
    t->pending_rect[2] = width;
    t->pending_rect[3] = height;
    t->has_pending_rect = 1;
}

/* update_anchor tells the input method daemon where the caret of the
 * active surface is on the screen. */
static void update_anchor(struct text_context *t)
{
    struct csurface *s = t->active;
    if (!s)
        return;
    if (t->has_rect)
        im_cursor_changed(s->x + t->rect[0], s->y + t->rect[1], t->rect[2], t->rect[3]);
    else
        im_cursor_changed(s->x + 8, s->y + 8, 1, 16);
}

static void h_commit(struct wire_client *c, struct wire_resource *self, uint32_t serial)
{
    struct text_context *t = self->data;
    t->serial = serial;
    if (t->has_pending_rect) {
        memcpy(t->rect, t->pending_rect, sizeof t->rect);
        t->has_rect = 1;
        t->has_pending_rect = 0;
        if (t->entered)
            update_anchor(t);
    }
    if (t->pending_action >= 0) {
        leave(t);
        t->active = t->pending_action ? t->pending : NULL;
        t->pending_action = -1;
        enter(t);
    }
    if (t->entered)
        update_anchor(t);
    text_input_send_done(self, serial);
    im_context_changed();
}

static void h_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static const struct text_input_impl text_handlers = {
    h_enable, h_disable, h_surrounding, h_content, h_commit, h_destroy, h_cursor_rectangle,
};

static void text_gone(struct wire_resource *r)
{
    struct text_context *t = r->data;
    if (t->client && t->client->text_input == r)
        t->client->text_input = NULL;
    free(t);
}

static void h_get_text_input(struct wire_client *c, struct wire_resource *self, uint32_t id,
                             struct wire_resource *seat)
{
    struct client *cl = wire_client_get_user_data(c);
    if (!cl || !seat || seat != cl->seat_res || cl->text_input) {
        wire_client_post_error(c, self, 42, "invalid or duplicate text input seat");
        return;
    }
    struct text_context *t = calloc(1, sizeof *t);
    struct wire_resource *r = t ? wire_resource_create(c, &text_input_interface, self->obj.version, id) : NULL;
    if (!r) {
        free(t);
        return;
    }
    t->res = r;
    t->client = cl;
    t->pending_action = -1;
    cl->text_input = r;
    wire_resource_set_listener(r, &text_handlers, t, text_gone);
}

static const struct text_input_manager_impl manager_handlers = { h_get_text_input };

static void bind_manager(struct wire_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wire_resource *r = wire_resource_create(c, &text_input_manager_interface, (int)version, id);
    if (r)
        wire_resource_set_listener(r, &manager_handlers, NULL, NULL);
}

void text_init(struct wire_server *srv)
{
    wire_global_create(srv, &text_input_manager_interface, 2, bind_manager, NULL);
}

void text_focus_changed(struct csurface *old, struct csurface *now)
{
    if (old)
        leave(context_of(old->client));
    if (now)
        enter(context_of(now->client));
}

static void commit_codepoint(struct text_context *t, uint32_t cp)
{
    char out[5];
    int n = gui_utf8_encode(cp, out);
    out[n] = '\0';
    text_input_send_commit_string(t->res, out);
    text_input_send_done(t->res, t->serial);
}

/* show_dead shows the accent of a waiting dead key as the preedit, and
 * clear_dead removes it. */
static void show_dead(struct text_context *t)
{
    char out[5] = "";
    int spacing = keymap_compose(seat_keymap(), t->dead, ' ');
    int n = spacing ? gui_utf8_encode((uint32_t)spacing, out) : 0;
    out[n] = '\0';
    text_input_send_preedit_string(t->res, out, n, n);
    text_input_send_done(t->res, t->serial);
}

static void clear_dead(struct text_context *t)
{
    t->dead = 0;
    text_input_send_preedit_string(t->res, "", 0, 0);
}

/* text_focus_active is 1 when the focused surface has an active text
 * input context, which receives the text of the typed keys. */
int text_focus_active(void)
{
    struct csurface *focus = seat_keyboard_focus();
    struct text_context *t = focus ? context_of(focus->client) : NULL;
    return t && t->entered && t->active == focus;
}

int text_key(uint32_t key, int pressed, int mods)
{
    struct csurface *focus = seat_keyboard_focus();
    struct text_context *t = focus ? context_of(focus->client) : NULL;
    if (!pressed || !t || !t->entered || t->active != focus)
        return 0;
    /* Ctrl+Shift+U starts Unicode hexadecimal entry. */
    if (!t->composing && (mods & (KEYMAP_MOD_CTRL | KEYMAP_MOD_SHIFT)) ==
        (KEYMAP_MOD_CTRL | KEYMAP_MOD_SHIFT) && key == 0x16) {
        t->composing = 1;
        t->nhex = 0;
        t->hex[0] = '\0';
        send_preedit(t);
        return 1;
    }
    if (t->composing) {
        if (key == 0x01) {
            end_preedit(t);
            return 1;
        }
        if (key == 0x0e) {
            if (t->nhex)
                t->hex[--t->nhex] = '\0';
            send_preedit(t);
            return 1;
        }
        if (key == 0x1c || key == 0x39) {
            uint32_t cp = 0;
            for (int i = 0; i < t->nhex; i++)
                cp = cp * 16 + (uint32_t)(t->hex[i] <= '9' ? t->hex[i] - '0' : t->hex[i] - 'a' + 10);
            end_preedit(t);
            if (t->nhex || cp)
                commit_codepoint(t, cp);
            return 1;
        }
        int ch = seat_translate(key, 0);
        if (t->nhex < 6 && ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
            if (ch >= 'A' && ch <= 'F') ch += 'a' - 'A';
            t->hex[t->nhex++] = (char)ch;
            t->hex[t->nhex] = '\0';
            send_preedit(t);
        }
        return 1;
    }
    if (mods & (KEYMAP_MOD_CTRL | KEYMAP_MOD_ALT))
        return 0;
    int ch = seat_translate(key, mods);
    /* A dead key waits for the next character.  The two compose one
     * character, or the accent and the character are committed when the
     * layout has no composition for them.  A second dead key commits the
     * accent of the first, and Escape or Backspace drops it. */
    if (keysym_is_dead(ch)) {
        if (t->dead) {
            int spacing = keymap_compose(seat_keymap(), t->dead, ' ');
            clear_dead(t);
            if (spacing)
                commit_codepoint(t, (uint32_t)spacing);
        }
        t->dead = ch;
        show_dead(t);
        return 0;
    }
    if (t->dead && (key == KEY_ESC || key == KEY_BACKSPACE)) {
        clear_dead(t);
        text_input_send_done(t->res, t->serial);
        return 1;
    }
    if (ch >= 32 && !keysym_is_symbol(ch)) {
        if (t->dead) {
            int composed = keymap_compose(seat_keymap(), t->dead, ch);
            int spacing = keymap_compose(seat_keymap(), t->dead, ' ');
            clear_dead(t);
            if (composed) {
                commit_codepoint(t, (uint32_t)composed);
                return 0;
            }
            if (spacing)
                commit_codepoint(t, (uint32_t)spacing);
        }
        commit_codepoint(t, (uint32_t)ch);
    }
    return 0;
}

int text_focused_state(const char **text, uint32_t *cursor, uint32_t *anchor, uint32_t *hints, uint32_t *purpose)
{
    struct csurface *focus = seat_keyboard_focus();
    struct text_context *t = focus ? context_of(focus->client) : NULL;
    if (!t || !t->entered || t->active != focus)
        return 0;
    if (text) *text = t->surrounding;
    if (cursor) *cursor = t->cursor;
    if (anchor) *anchor = t->anchor;
    if (hints) *hints = t->hints;
    if (purpose) *purpose = t->purpose;
    return 1;
}

void text_im_apply(const char *commit, const char *preedit, int begin, int end, uint32_t before, uint32_t after)
{
    struct csurface *focus = seat_keyboard_focus();
    struct text_context *t = focus ? context_of(focus->client) : NULL;
    if (!t || !t->entered || t->active != focus)
        return;
    if (before || after)
        text_input_send_delete_surrounding_text(t->res, before, after);
    if (commit && commit[0])
        text_input_send_commit_string(t->res, commit);
    if (preedit)
        text_input_send_preedit_string(t->res, preedit, begin, end);
    text_input_send_done(t->res, t->serial);
}

void text_surface_gone(struct csurface *s)
{
    struct text_context *t = context_of(s ? s->client : NULL);
    if (!t)
        return;
    if (t->active == s) {
        t->active = NULL;
        t->entered = 0;
        im_update();
    }
    if (t->pending == s)
        t->pending = NULL;
}

void text_client_gone(struct client *c)
{
    if (c)
        c->text_input = NULL;
}

