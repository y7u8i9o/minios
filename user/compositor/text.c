/* Text input and X12's small built-in Unicode composer. Physical keys
 * remain on the keyboard interface; committed text and preedit state use
 * this protocol so clients do not have to infer text from scancodes. */
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

static struct text_context *context_of(struct client *cl)
{
    return cl && cl->text_input ? cl->text_input->data : NULL;
}

static void leave(struct text_context *t)
{
    if (t && t->entered) {
        end_preedit(t);
        text_input_send_leave(t->res, t->active->res);
        t->entered = 0;
    }
}

static void enter(struct text_context *t)
{
    if (t && t->active && seat_keyboard_focus() == t->active && !t->entered) {
        text_input_send_enter(t->res, t->active->res);
        t->entered = 1;
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
    size_t kept = strlen(t->surrounding);
    t->cursor = cursor <= kept ? cursor : (uint32_t)kept;
    t->anchor = anchor <= kept ? anchor : (uint32_t)kept;
}

static void h_content(struct wire_client *c, struct wire_resource *self, uint32_t hints, uint32_t purpose)
{
    struct text_context *t = self->data;
    t->hints = hints;
    t->purpose = purpose;
}

static void h_commit(struct wire_client *c, struct wire_resource *self, uint32_t serial)
{
    struct text_context *t = self->data;
    t->serial = serial;
    if (t->pending_action >= 0) {
        leave(t);
        t->active = t->pending_action ? t->pending : NULL;
        t->pending_action = -1;
        enter(t);
    }
    text_input_send_done(self, serial);
}

static void h_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static const struct text_input_impl text_handlers = {
    h_enable, h_disable, h_surrounding, h_content, h_commit, h_destroy,
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
    struct wire_resource *r = t ? wire_resource_create(c, &text_input_interface, 1, id) : NULL;
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
    wire_global_create(srv, &text_input_manager_interface, 1, bind_manager, NULL);
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

void text_key(uint32_t key, int pressed, int mods)
{
    struct csurface *focus = seat_keyboard_focus();
    struct text_context *t = focus ? context_of(focus->client) : NULL;
    if (!pressed || !t || !t->entered || t->active != focus)
        return;
    /* Ctrl+Shift+U starts Unicode hexadecimal entry. */
    if (!t->composing && (mods & (KEYMAP_MOD_CTRL | KEYMAP_MOD_SHIFT)) ==
        (KEYMAP_MOD_CTRL | KEYMAP_MOD_SHIFT) && key == 0x16) {
        t->composing = 1;
        t->nhex = 0;
        t->hex[0] = '\0';
        send_preedit(t);
        return;
    }
    if (t->composing) {
        if (key == 0x01) {
            end_preedit(t);
            return;
        }
        if (key == 0x0e) {
            if (t->nhex)
                t->hex[--t->nhex] = '\0';
            send_preedit(t);
            return;
        }
        if (key == 0x1c || key == 0x39) {
            uint32_t cp = 0;
            for (int i = 0; i < t->nhex; i++)
                cp = cp * 16 + (uint32_t)(t->hex[i] <= '9' ? t->hex[i] - '0' : t->hex[i] - 'a' + 10);
            end_preedit(t);
            if (t->nhex || cp)
                commit_codepoint(t, cp);
            return;
        }
        int ch = seat_translate(key, 0);
        if (t->nhex < 6 && ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
            if (ch >= 'A' && ch <= 'F') ch += 'a' - 'A';
            t->hex[t->nhex++] = (char)ch;
            t->hex[t->nhex] = '\0';
            send_preedit(t);
        }
        return;
    }
    if (mods & (KEYMAP_MOD_CTRL | KEYMAP_MOD_ALT))
        return;
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
        return;
    }
    if (t->dead && (key == KEY_ESC || key == KEY_BACKSPACE)) {
        clear_dead(t);
        text_input_send_done(t->res, t->serial);
        return;
    }
    if (ch >= 32 && !keysym_is_symbol(ch)) {
        if (t->dead) {
            int composed = keymap_compose(seat_keymap(), t->dead, ch);
            int spacing = keymap_compose(seat_keymap(), t->dead, ' ');
            clear_dead(t);
            if (composed) {
                commit_codepoint(t, (uint32_t)composed);
                return;
            }
            if (spacing)
                commit_codepoint(t, (uint32_t)spacing);
        }
        commit_codepoint(t, (uint32_t)ch);
    }
}

void text_surface_gone(struct csurface *s)
{
    struct text_context *t = context_of(s ? s->client : NULL);
    if (!t)
        return;
    if (t->active == s) {
        t->active = NULL;
        t->entered = 0;
    }
    if (t->pending == s)
        t->pending = NULL;
}

void text_client_gone(struct client *c)
{
    if (c)
        c->text_input = NULL;
}

