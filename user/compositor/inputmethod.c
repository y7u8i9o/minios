/* The input methods of the seat (I1, docs/design/ime.md): the list of
 * methods that the switch keys select, and the relay between the text
 * input contexts and the input method daemon imed.
 *
 * Method 0 is the keyboard layout.  The engines of the daemon follow in
 * the order of its set_engines request, then the built-in Japanese engine
 * of ime.c (L6), which the daemon replaces in I4.  While an engine of the daemon is
 * selected and a text input context has the keyboard focus, the daemon is
 * active: it receives every key of the context and replies whether it
 * used it.  The keys wait in a queue for the reply, at most 150 ms each,
 * and the queue keeps their order with the keys typed after them.
 *
 * The state below belongs to the single thread of the compositor. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gui/keymap.h>
#include <minios/input.h>
#include "comp.h"

#define MAX_METHODS 12
#define MAX_QUEUE 32
#define KEY_TIMEOUT_MS 150

struct method {
    char name[32], label[16], title[64];
    int builtin;                /* IME_JAPANESE of ime.c, 0 for the layout or the daemon */
};

struct pending_key {
    uint32_t serial, key;
    int pressed, mods;
    long time;
    int decided, handled;
};

static struct method methods[MAX_METHODS];
static int nmethods, current, last_engine;
static int engine_chosen;                         /* the user selected an engine: last_engine stays */
static char daemon_engines[MAX_METHODS][3][64];   /* name, label and title of each engine of the daemon */
static int ndaemon;
static struct wire_resource *im;                  /* the input method of the daemon, or NULL */
static struct wire_resource *controls[8];
static int ncontrols;
static int active;
static uint32_t done_serial, key_serial;
static struct pending_key queue[MAX_QUEUE];
static int nqueue;
static uint32_t handled_keys[16];                 /* presses that the daemon used: their release is dropped */
static int nhandled;
/* The text changes of the daemon that its next commit applies. */
static char pending_commit[1024], pending_preedit[512];
static int pending_preedit_set, pending_begin, pending_end;
static uint32_t pending_before, pending_after;
static struct csurface *candidates;               /* the surface of the candidate window */
static struct csurface *composed;                 /* the focused surface when the daemon was last active */
static struct wire_resource *candidates_res;
static int cursor_rect[4] = { 0, 0, 0, 16 };

/* ---- the methods ---- */

static void add_method(const char *name, const char *label, const char *title, int builtin)
{
    if (nmethods == MAX_METHODS)
        return;
    struct method *m = &methods[nmethods++];
    strlcpy(m->name, name, sizeof m->name);
    strlcpy(m->label, label, sizeof m->label);
    strlcpy(m->title, title, sizeof m->title);
    m->builtin = builtin;
}

/* rebuild_methods makes the list again after the daemon came, went or
 * changed its engines.  The current method stays selected by its name. */
static void rebuild_methods(void)
{
    char was[32] = "", last[32] = "";
    if (nmethods) {
        strlcpy(was, methods[current].name, sizeof was);
        strlcpy(last, methods[last_engine].name, sizeof last);
    }
    nmethods = 0;
    add_method("layout", "", "Keyboard layout", 0);
    for (int i = 0; i < ndaemon; i++)
        add_method(daemon_engines[i][0], daemon_engines[i][1], daemon_engines[i][2], 0);
    add_method("l6-japanese", "あ", "Japanese (characters)", IME_JAPANESE);
    current = 0;
    last_engine = -1;
    for (int i = 0; i < nmethods; i++) {
        if (strcmp(methods[i].name, was) == 0)
            current = i;
        if (engine_chosen && strcmp(methods[i].name, last) == 0)
            last_engine = i;
    }
    /* The first Chinese engine is the engine of the first toggle. */
    for (int i = 1; i < nmethods && last_engine < 0; i++)
        if (strstr(methods[i].name, "pinyin") || strstr(methods[i].name, "chinese"))
            last_engine = i;
    if (last_engine < 0)
        last_engine = nmethods > 1 ? 1 : 0;
}

void im_label(char *out, size_t size)
{
    if (current == 0 || !methods[current].label[0])
        seat_layout_label(out, size);
    else
        strlcpy(out, methods[current].label, size);
}

static void send_engines(struct wire_resource *r)
{
    char list[1024] = "", line[160];
    char layout[16];
    seat_layout_label(layout, sizeof layout);
    for (int i = 0; i < nmethods; i++) {
        snprintf(line, sizeof line, "%s\t%s\t%s\n", methods[i].name, i == 0 ? layout : methods[i].label,
                 methods[i].title);
        strlcat(list, line, sizeof list);
    }
    ime_control_send_engines(r, list);
}

static void send_current(struct wire_resource *r)
{
    char label[16];
    im_label(label, sizeof label);
    ime_control_send_current(r, methods[current].name, label);
}

/* im_status_changed tells the panel and the controls the current method
 * and its label. */
static void im_status_changed(void)
{
    for (int i = 0; i < ncontrols; i++)
        send_current(controls[i]);
    seat_input_label_changed();
}

static void engines_changed(void)
{
    for (int i = 0; i < ncontrols; i++)
        send_engines(controls[i]);
    im_status_changed();
}

/* ---- activation ---- */

static void flush_queue(void);

static void send_context_state(void)
{
    const char *text;
    uint32_t cursor, anchor, hints, purpose;
    if (text_focused_state(&text, &cursor, &anchor, &hints, &purpose)) {
        input_method_send_surrounding_text(im, text, cursor, anchor);
        input_method_send_content_type(im, hints, purpose);
    }
    input_method_send_cursor_rectangle(im, cursor_rect[0], cursor_rect[1], cursor_rect[2], cursor_rect[3]);
}

static void candidates_damage(void)
{
    if (candidates && candidates->mapped)
        scene_damage(surface_rect(candidates));
}

/* im_update activates or deactivates the daemon after a change of the
 * method, of the keyboard focus or of a text input context. */
void im_update(void)
{
    int want = im && current > 0 && !methods[current].builtin && text_focused_state(NULL, NULL, NULL, NULL, NULL);
    if (want == active)
        return;
    active = want;
    candidates_damage();
    if (active) {
        composed = seat_keyboard_focus();
        input_method_send_activate(im);
        send_context_state();
    } else {
        for (int i = 0; i < nqueue; i++)
            queue[i].decided = 1;
        flush_queue();
        nhandled = 0;
        if (im)
            input_method_send_deactivate(im);
    }
    if (im)
        input_method_send_done(im, ++done_serial);
    comp_debug("ime: %s", active ? "active" : "inactive");
}

/* im_select selects method index, or the next one for -1.  A composition
 * of a built-in engine is committed as it is shown.  The daemon commits
 * its own composition when another engine is selected or when it is
 * deactivated. */
void im_select(int index)
{
    if (!nmethods)
        rebuild_methods();
    if (index < 0)
        index = (current + 1) % nmethods;
    if (index >= nmethods || index == current)
        return;
    if (methods[current].builtin)
        text_ime_end(1);
    /* An engine of the daemon commits its composition when another
     * method is selected. */
    if (im && current > 0 && !methods[current].builtin && (index == 0 || methods[index].builtin))
        input_method_send_select_engine(im, methods[index].name);
    current = index;
    if (current > 0) {
        last_engine = current;
        engine_chosen = 1;
    }
    ime_set_mode(methods[current].builtin);
    if (im && current > 0 && !methods[current].builtin)
        input_method_send_select_engine(im, methods[current].name);
    im_update();
    char label[16];
    im_label(label, sizeof label);
    comp_log("input method %s", label);
    im_status_changed();
}

/* A Shift tap, Ctrl+Space and Super+Space toggle between the layout and
 * the last engine. */
void im_toggle(void)
{
    if (!nmethods)
        rebuild_methods();
    im_select(current == 0 ? last_engine : 0);
}

static void select_by_name(const char *name)
{
    for (int i = 0; i < nmethods; i++)
        if (strcmp(methods[i].name, name) == 0) {
            im_select(i);
            return;
        }
}

/* ---- keys ---- */

static int take_handled(uint32_t key)
{
    for (int i = 0; i < nhandled; i++)
        if (handled_keys[i] == key) {
            handled_keys[i] = handled_keys[--nhandled];
            return 1;
        }
    return 0;
}

/* flush_queue delivers or drops the decided keys at the head. */
static void flush_queue(void)
{
    while (nqueue && queue[0].decided) {
        struct pending_key k = queue[0];
        memmove(queue, queue + 1, (size_t)(--nqueue) * sizeof queue[0]);
        if (k.pressed && k.handled) {
            if (nhandled < 16)
                handled_keys[nhandled++] = k.key;
        } else if (!k.pressed && take_handled(k.key)) {
            continue;
        } else {
            seat_deliver_key(k.key, k.pressed, k.mods);
        }
    }
}

/* im_filter_key returns 1 when the key waits for the daemon or was used
 * by it, and 0 when the seat delivers it now. */
int im_filter_key(uint32_t key, int pressed, int mods)
{
    if (!pressed && !nqueue && take_handled(key))
        return 1;
    if (!active)
        return 0;
    long now = uptime_ms();
    if (!pressed) {
        if (!nqueue)
            return 0;
        if (nqueue == MAX_QUEUE)
            return 0;
        queue[nqueue++] = (struct pending_key){ 0, key, 0, mods, now, 1, 0 };
        return 1;
    }
    /* A shortcut goes to the client at once.  The daemon learns of it and
     * commits its composition. */
    if ((mods & (KEYMAP_MOD_CTRL | KEYMAP_MOD_ALT | KEYMAP_MOD_LOGO)) && !nqueue) {
        input_method_send_key(im, 0, (uint32_t)now, key, 1);
        return 0;
    }
    if (nqueue == MAX_QUEUE)
        return 0;
    uint32_t serial = ++key_serial ? key_serial : ++key_serial;
    queue[nqueue++] = (struct pending_key){ serial, key, 1, mods, now, 0, 0 };
    input_method_send_key(im, serial, (uint32_t)now, key, 1);
    return 1;
}

void im_tick(long now)
{
    if (nqueue && !queue[0].decided && now - queue[0].time > KEY_TIMEOUT_MS) {
        comp_log("ime: key 0x%02x timed out", queue[0].key);
        queue[0].decided = 1;
        flush_queue();
    }
}

void im_modifiers(int depressed, int locked, int group)
{
    if (im)
        input_method_send_modifiers(im, (uint32_t)depressed, (uint32_t)locked, (uint32_t)group);
}

void im_keymap_changed(int fd, uint32_t size)
{
    if (im && fd >= 0)
        input_method_send_keymap(im, 1, fd, size);
    rebuild_methods();
    engines_changed();
}

/* ---- the context ---- */

void im_cursor_changed(int x, int y, int width, int height)
{
    if (cursor_rect[0] == x && cursor_rect[1] == y && cursor_rect[2] == width && cursor_rect[3] == height)
        return;
    cursor_rect[0] = x;
    cursor_rect[1] = y;
    cursor_rect[2] = width;
    cursor_rect[3] = height;
    if (active) {
        input_method_send_cursor_rectangle(im, x, y, width, height);
        input_method_send_done(im, ++done_serial);
    }
    im_place_candidates();
}

void im_context_changed(void)
{
    im_update();
    if (active) {
        send_context_state();
        input_method_send_done(im, ++done_serial);
    }
}

/* ---- the candidate window ---- */

int im_candidates_visible(void)
{
    return active;
}

/* im_place_candidates puts the window below the caret, or above it when
 * the screen ends below, inside the screen. */
void im_place_candidates(void)
{
    struct csurface *s = candidates;
    if (!s || !s->mapped)
        return;
    int x = cursor_rect[0], y = cursor_rect[1] + cursor_rect[3] + 2;
    if (y + s->height > screen_h)
        y = cursor_rect[1] - s->height - 2;
    if (x + s->width > screen_w)
        x = screen_w - s->width;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x == s->x && y == s->y)
        return;
    candidates_damage();
    s->x = x;
    s->y = y;
    candidates_damage();
    candidate_surface_send_placed(candidates_res, x, y);
    comp_log("ime: candidates at %d,%d", x, y);
}

void im_candidates_committed(struct csurface *s, int first_map)
{
    static int logged;
    if (first_map && !logged) {
        comp_log("ime: candidate surface %d mapped %dx%d", s->id, s->width, s->height);
        logged = 1;
    }
    im_place_candidates();
}

void im_surface_gone(struct csurface *s)
{
    if (s == composed)
        composed = NULL;
    if (s == candidates) {
        candidates = NULL;
        if (candidates_res)
            candidates_res->data = NULL;
    }
}

static void h_candidates_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static const struct candidate_surface_impl candidate_handlers = { h_candidates_destroy };

static void candidates_gone(struct wire_resource *r)
{
    struct csurface *s = r->data;
    if (s && s == candidates) {
        candidates_damage();
        s->role = ROLE_NONE;
        s->mapped = 0;
        candidates = NULL;
    }
    if (candidates_res == r)
        candidates_res = NULL;
}

/* ---- the input method ---- */

static void h_key_handled(struct wire_client *c, struct wire_resource *self, uint32_t serial, uint32_t handled)
{
    for (int i = 0; i < nqueue; i++)
        if (queue[i].serial == serial && !queue[i].decided) {
            queue[i].decided = 1;
            queue[i].handled = handled != 0;
            break;
        }
    flush_queue();
}

static void h_commit_string(struct wire_client *c, struct wire_resource *self, const char *text)
{
    strlcat(pending_commit, text, sizeof pending_commit);
}

static void h_preedit_string(struct wire_client *c, struct wire_resource *self, const char *text, int32_t begin,
                             int32_t end)
{
    strlcpy(pending_preedit, text, sizeof pending_preedit);
    pending_preedit_set = 1;
    pending_begin = begin;
    pending_end = end;
}

static void h_delete_surrounding(struct wire_client *c, struct wire_resource *self, uint32_t before, uint32_t after)
{
    pending_before = before;
    pending_after = after;
}

/* The changes apply to the focused context while the daemon is active, and
 * after a change of method as long as the same surface has the focus. */
static void h_commit(struct wire_client *c, struct wire_resource *self, uint32_t serial)
{
    if (active || (composed && composed == seat_keyboard_focus()))
        text_im_apply(pending_commit, pending_preedit_set ? pending_preedit : NULL, pending_begin, pending_end,
                      pending_before, pending_after);
    pending_commit[0] = pending_preedit[0] = '\0';
    pending_preedit_set = 0;
    pending_before = pending_after = 0;
}

/* set_engines: one line "name TAB label TAB title" per engine. */
static void h_set_engines(struct wire_client *c, struct wire_resource *self, const char *list)
{
    ndaemon = 0;
    while (*list && ndaemon < MAX_METHODS - 3) {
        const char *end = strchr(list, '\n');
        size_t n = end ? (size_t)(end - list) : strlen(list);
        char line[200];
        if (n >= sizeof line)
            n = sizeof line - 1;
        memcpy(line, list, n);
        line[n] = '\0';
        char *label = strchr(line, '\t'), *title = label ? strchr(label + 1, '\t') : NULL;
        if (label && title) {
            *label++ = '\0';
            *title++ = '\0';
            strlcpy(daemon_engines[ndaemon][0], line, sizeof daemon_engines[0][0]);
            strlcpy(daemon_engines[ndaemon][1], label, sizeof daemon_engines[0][1]);
            strlcpy(daemon_engines[ndaemon][2], title, sizeof daemon_engines[0][2]);
            ndaemon++;
        }
        list += n + (end ? 1 : 0);
    }
    rebuild_methods();
    comp_log("ime: %d engines", ndaemon);
    engines_changed();
    im_update();
}

static void h_set_status(struct wire_client *c, struct wire_resource *self, const char *engine, const char *label)
{
    for (int i = 1; i < nmethods; i++)
        if (strcmp(methods[i].name, engine) == 0 && strcmp(methods[i].label, label) != 0) {
            strlcpy(methods[i].label, label, sizeof methods[i].label);
            if (i == current)
                im_status_changed();
        }
}

static void h_get_candidate_surface(struct wire_client *c, struct wire_resource *self, uint32_t id,
                                    struct wire_resource *surface)
{
    struct csurface *s = surface ? surface->data : NULL;
    if (!s || s->role != ROLE_NONE || candidates) {
        wire_client_post_error(c, self, 50, "the surface has a role or a candidate surface exists");
        return;
    }
    struct wire_resource *r = wire_resource_create(c, &candidate_surface_interface, 1, id);
    if (!r)
        return;
    s->role = ROLE_IME_POPUP;
    candidates = s;
    candidates_res = r;
    wire_resource_set_listener(r, &candidate_handlers, s, candidates_gone);
}

static void h_im_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }

static const struct input_method_impl im_handlers = {
    h_key_handled, h_commit_string, h_preedit_string, h_delete_surrounding, h_commit, h_set_engines,
    h_set_status, h_get_candidate_surface, h_im_destroy,
};

static void im_gone(struct wire_resource *r)
{
    if (im != r)
        return;
    for (int i = 0; i < nqueue; i++)
        queue[i].decided = 1;
    flush_queue();
    nhandled = 0;
    im = NULL;
    active = 0;
    ndaemon = 0;
    rebuild_methods();
    ime_set_mode(methods[current].builtin);
    comp_log("ime: input method gone");
    engines_changed();
}

static void h_get_input_method(struct wire_client *c, struct wire_resource *self, uint32_t id,
                               struct wire_resource *seat)
{
    if (im) {
        wire_client_post_error(c, self, 51, "another client holds the input method");
        return;
    }
    struct wire_resource *r = wire_resource_create(c, &input_method_interface, 1, id);
    if (!r)
        return;
    im = r;
    wire_resource_set_listener(r, &im_handlers, NULL, im_gone);
    uint32_t size;
    int fd = seat_keymap_fd(&size);
    if (fd >= 0)
        input_method_send_keymap(r, 1, fd, size);
    comp_log("ime: input method bound");
}

/* ---- controls ---- */

static void h_control_select(struct wire_client *c, struct wire_resource *self, const char *name)
{
    select_by_name(name);
}
static void h_control_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static const struct ime_control_impl control_handlers = { h_control_select, h_control_destroy };

static void control_gone(struct wire_resource *r)
{
    for (int i = 0; i < ncontrols; i++)
        if (controls[i] == r) {
            controls[i] = controls[--ncontrols];
            return;
        }
}

static void h_get_control(struct wire_client *c, struct wire_resource *self, uint32_t id)
{
    struct wire_resource *r = wire_resource_create(c, &ime_control_interface, 1, id);
    if (!r)
        return;
    wire_resource_set_listener(r, &control_handlers, NULL, control_gone);
    if (ncontrols < 8)
        controls[ncontrols++] = r;
    if (!nmethods)
        rebuild_methods();
    send_engines(r);
    send_current(r);
}

static const struct input_method_manager_impl manager_handlers = { h_get_input_method, h_get_control };

static void bind_manager(struct wire_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wire_resource *r = wire_resource_create(c, &input_method_manager_interface, (int)version, id);
    if (r)
        wire_resource_set_listener(r, &manager_handlers, NULL, NULL);
}

void im_init(struct wire_server *srv)
{
    rebuild_methods();
    wire_global_create(srv, &input_method_manager_interface, 1, bind_manager, NULL);
}
