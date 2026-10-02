/* The seat: pointer focus and events, keyboard focus and events with
 * the keymap descriptor, modifiers, serials, cursor surfaces, and the
 * compositor's own shortcuts (Alt+Tab, Alt+F4, Alt drag). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <gui/keymap.h>
#include <minios/input.h>
#include <gui/mime.h>
#include "comp.h"

int decor_hit(const struct csurface *s, int x, int y);

static struct csurface *pointer_focus, *keyboard_focus, *press_focus, *cursor_surface;
static int buttons, modifiers;
static uint8_t mod_shift, mod_ctrl, mod_alt, mod_logo;   /* bit 0 left key, bit 1 right key */
static uint32_t last_serial;
static uint32_t cursor_serial, grab_serial;
static struct client *cursor_client, *grab_client;
static struct csurface *grab_origin;
static int cursor_is_hidden;
static struct wire_server *server_of_seat;
static int keymap_fd = -1;
static uint32_t keymap_size;
static struct keymap *server_keymap;
static uint32_t pressed_keys[16];
static int npressed;

uint32_t seat_last_serial(void) { return last_serial; }
int seat_modifiers(void) { return modifiers; }
struct csurface *seat_keyboard_focus(void) { return keyboard_focus; }
struct csurface *seat_cursor_surface(void) { return cursor_surface; }
int seat_cursor_hidden(void) { return cursor_is_hidden; }
int seat_translate(uint32_t key, int mods) { return keymap_translate(server_keymap, key, mods); }

static uint32_t serial(void)
{
    last_serial = comp_serial();
    return last_serial;
}

static uint32_t serial_for(struct client *cl)
{
    uint32_t n = serial();
    if (cl) {
        if (cl->ninput_serials < 16)
            cl->input_serials[cl->ninput_serials++] = n;
        else {
            memmove(cl->input_serials, cl->input_serials + 1, 15 * sizeof cl->input_serials[0]);
            cl->input_serials[15] = n;
        }
    }
    return n;
}

int seat_validate_serial(struct client *client, uint32_t n)
{
    if (!client || !n)
        return 0;
    for (int i = 0; i < client->ninput_serials; i++)
        if (client->input_serials[i] == n)
            return 1;
    return 0;
}

int seat_buttons(void)
{
    return buttons;
}

int seat_validate_grab(struct client *client, struct csurface *origin, uint32_t n)
{
    return n != 0 && n == grab_serial && client == grab_client && (!origin || origin == grab_origin);
}

int seat_validate_drag(struct client *client, struct csurface *origin, uint32_t n)
{
    if (seat_validate_grab(client, origin, n))
        return 1;
    /* Older clients start a drag from the pointer-enter serial rather than
     * waiting for a button event. Keep that form safe: it is accepted only
     * while the pointer still targets the origin and no button is held. */
    return client && origin && origin == pointer_focus && origin->client == client &&
           !buttons && seat_validate_serial(client, n);
}

static uint32_t now_ms(void) { return (uint32_t)uptime_ms(); }

/* Pointer coordinates relative to a surface, with the fraction of the
 * logical pixel the cursor is at. */
static int32_t fixed_x(const struct csurface *s) { return (int32_t)((cursor_fx - s->x) * 256.0); }
static int32_t fixed_y(const struct csurface *s) { return (int32_t)((cursor_fy - s->y) * 256.0); }

/* ---- resources ---- */

static void h_set_cursor(struct wire_client *c, struct wire_resource *self, uint32_t serial_, struct wire_resource *surface, int32_t hx, int32_t hy)
{
    struct client *cl = wire_client_get_user_data(c);
    if (!cl || serial_ != cursor_serial || cl != cursor_client || pointer_focus == NULL || pointer_focus->client != cl)
        return;
    scene_cursor_changed();
    if (!surface) {
        cursor_surface = NULL;
        cursor_is_hidden = 1;
        scene_cursor_changed();
        return;
    }
    struct csurface *s = surface->data;
    if (s->client != cl || (s->role != ROLE_NONE && s->role != ROLE_CURSOR)) {
        scene_cursor_changed();
        return;
    }
    if (s->role == ROLE_NONE) {
        s->role = ROLE_CURSOR;
        comp_debug("cursor surface %d", s->id);
    }
    s->hotspot_x = hx;
    s->hotspot_y = hy;
    cursor_surface = s;
    cursor_is_hidden = 0;
    s->x = cursor_x - hx;
    s->y = cursor_y - hy;
    scene_cursor_changed();
}
static void h_pointer_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static const struct pointer_impl pointer_handlers = { h_set_cursor, h_pointer_destroy };
static void pointer_gone(struct wire_resource *r)
{
    struct client *cl = wire_client_get_user_data(r->client);
    if (cl && cl->pointer == r)
        cl->pointer = NULL;
}

static void h_keyboard_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static const struct keyboard_impl keyboard_handlers = { h_keyboard_destroy };
static void keyboard_gone(struct wire_resource *r)
{
    struct client *cl = wire_client_get_user_data(r->client);
    if (cl && cl->keyboard == r)
        cl->keyboard = NULL;
}

static void send_keyboard_enter(struct client *cl, struct csurface *s)
{
    struct wire_array keys = { pressed_keys, sizeof(uint32_t) * (size_t)npressed };
    keyboard_send_enter(cl->keyboard, serial_for(cl), s->res, &keys);
    keyboard_send_modifiers(cl->keyboard, last_serial, (uint32_t)modifiers, 0, 0, 0);
}

static void h_get_pointer(struct wire_client *c, struct wire_resource *self, uint32_t id)
{
    struct client *cl = wire_client_get_user_data(c);
    struct wire_resource *r = wire_resource_create(c, &pointer_interface, 1, id);
    if (!r || !cl)
        return;
    wire_resource_set_listener(r, &pointer_handlers, NULL, pointer_gone);
    cl->pointer = r;
    if (pointer_focus && pointer_focus->client == cl) {
        uint32_t n = serial_for(cl);
        cursor_serial = n;
        cursor_client = cl;
        pointer_send_enter(r, n, pointer_focus->res, fixed_x(pointer_focus), fixed_y(pointer_focus));
    }
}

static void h_get_keyboard(struct wire_client *c, struct wire_resource *self, uint32_t id)
{
    struct client *cl = wire_client_get_user_data(c);
    struct wire_resource *r = wire_resource_create(c, &keyboard_interface, 1, id);
    if (!r || !cl)
        return;
    wire_resource_set_listener(r, &keyboard_handlers, NULL, keyboard_gone);
    cl->keyboard = r;
    if (keymap_fd >= 0)
        keyboard_send_keymap(r, 1, keymap_fd, keymap_size);
    keyboard_send_repeat_info(r, settings.repeat_rate, settings.repeat_delay);
    if (keyboard_focus && keyboard_focus->client == cl)
        send_keyboard_enter(cl, keyboard_focus);
}
static const struct seat_impl seat_handlers = { h_get_pointer, h_get_keyboard };

static void bind_seat(struct wire_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wire_resource *r = wire_resource_create(c, &seat_interface, (int)version, id);
    if (!r)
        return;
    struct client *cl = wire_client_get_user_data(c);
    if (cl)
        cl->seat_res = r;
    wire_resource_set_listener(r, &seat_handlers, NULL, NULL);
    seat_send_capabilities(r, 3);
    seat_send_name(r, "seat0");
}

/* The keymap file is copied into a memfd, which clients map. */
int seat_load_keymap(const char *name)
{
    char path[128];
    snprintf(path, sizeof path, "/usr/share/keymaps/%s.mkm", name);
    struct keymap *k = keymap_load(path);
    if (!k)
        return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        keymap_free(k);
        return -1;
    }
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    if (n <= 0) {
        keymap_free(k);
        return -1;
    }
    int mfd = memfd_create("keymap", MFD_CLOEXEC);
    if (mfd < 0 || ftruncate(mfd, 4096) < 0) {
        keymap_free(k);
        return -1;
    }
    void *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (map == MAP_FAILED) {
        close(mfd);
        keymap_free(k);
        return -1;
    }
    memcpy(map, buf, (size_t)n);
    munmap(map, 4096);
    if (server_keymap)
        keymap_free(server_keymap);
    if (keymap_fd >= 0)
        close(keymap_fd);
    server_keymap = k;
    keymap_fd = mfd;
    keymap_size = (uint32_t)n;
    comp_log("keymap %s", name);
    return 0;
}

void seat_init(struct wire_server *srv)
{
    seat_load_keymap("us");
    server_of_seat = srv;
    wire_global_create(srv, &seat_interface, 1, bind_seat, NULL);
}

/* ---- pointer ---- */

static void set_pointer_focus(struct csurface *s)
{
    if (pointer_focus == s)
        return;
    if (pointer_focus && pointer_focus->client->pointer) {
        pointer_send_leave(pointer_focus->client->pointer, serial_for(pointer_focus->client), pointer_focus->res);
        pointer_send_frame(pointer_focus->client->pointer);
    }
    pointer_focus = s;
    if (s && s->client->pointer) {
        uint32_t n = serial_for(s->client);
        cursor_serial = n;
        cursor_client = s->client;
        pointer_send_enter(s->client->pointer, n, s->res, fixed_x(s), fixed_y(s));
        pointer_send_frame(s->client->pointer);
        comp_debug("pointer enter surface %d", s->id);
    }
}

void seat_pointer_motion(void)
{
    if (data_dragging()) {
        data_pointer_motion();
        return;
    }
    if (decor_motion())
        return;
    struct csurface *target = press_focus ? press_focus : scene_surface_at(cursor_x, cursor_y);
    if (!press_focus) {
        struct csurface *grab = popup_grab_surface();
        if (grab && target && target != grab && target->role != ROLE_POPUP)
            target = NULL;
    }
    set_pointer_focus(target);
    if (target && target->client->pointer) {
        pointer_send_motion(target->client->pointer, now_ms(), fixed_x(target), fixed_y(target));
        pointer_send_frame(target->client->pointer);
    }
}

void seat_pointer_button(int button, int pressed)
{
    int bit = 1 << (button - 1);
    buttons = pressed ? buttons | bit : buttons & ~bit;
    if (pressed) {
        if (data_dragging())
            return;
        if (hang_press(cursor_x, cursor_y))
            return;
        struct csurface *grab = popup_grab_surface();
        struct csurface *target = scene_surface_at(cursor_x, cursor_y);
        if (grab && (!target || target->role != ROLE_POPUP)) {
            popup_dismiss_all();
            return;
        }
        /* Decorations of the toplevel under the cursor. */
        for (struct csurface *s = surface_first(); s; s = s->next)
            if (decor_hit(s, cursor_x, cursor_y) && !s->toplevel->minimized && (!target || target->stack <= s->stack)) {
                if (toplevel_blocked(s->toplevel)) {
                    set_pointer_focus(NULL);
                    return;
                }
                toplevel_activate(s->toplevel);
                decor_press(s, button);
                return;
            }
        if (target) {
            if (target->role == ROLE_TOPLEVEL && toplevel_blocked(target->toplevel)) {
                set_pointer_focus(NULL);
                return;
            } else if (target->role == ROLE_TOPLEVEL)
                toplevel_activate(target->toplevel);
            else if (target->role == ROLE_LAYER && target->layer->interactive)
                seat_set_keyboard_focus(target);
            press_focus = target;
            if (button == 1 && (modifiers & KEYMAP_MOD_ALT) && target->role == ROLE_TOPLEVEL) {
                decor_press(target, 1);
                press_focus = NULL;
                return;
            }
        }
        set_pointer_focus(target);
    } else {
        if (data_dragging()) {
            data_pointer_release();
            return;
        }
        /* A drag the client asked for still owes it the release. */
        if (decor_release() == 1)
            return;
        if (!buttons)
            press_focus = NULL;
    }
    struct csurface *t = pointer_focus;
    if (t && t->client->pointer) {
        uint32_t n = serial_for(t->client);
        if (pressed) {
            grab_serial = n;
            grab_client = t->client;
            grab_origin = t;
        }
        pointer_send_button(t->client->pointer, n, now_ms(), (uint32_t)button, pressed ? 1 : 0);
        pointer_send_frame(t->client->pointer);
        comp_debug("button %d %s in surface %d", button, pressed ? "down" : "up", t->id);
    }
}

void seat_pointer_axis(int value)
{
    struct csurface *t = scene_surface_at(cursor_x, cursor_y);
    if (t && t->client->pointer) {
        pointer_send_axis(t->client->pointer, now_ms(), 0, value);
        pointer_send_frame(t->client->pointer);
        comp_debug("axis %d in surface %d", value / (15 * 256), t->id);
    }
}

/* ---- keyboard ---- */

void seat_set_keyboard_focus(struct csurface *s)
{
    if (keyboard_focus == s)
        return;
    struct csurface *old = keyboard_focus;
    if (keyboard_focus && keyboard_focus->client->keyboard)
        keyboard_send_leave(keyboard_focus->client->keyboard, serial_for(keyboard_focus->client), keyboard_focus->res);
    keyboard_focus = s;
    text_focus_changed(old, s);
    if (s) {
        if (s->client->keyboard)
            send_keyboard_enter(s->client, s);
        comp_debug("keyboard focus surface %d", s->id);
        data_keyboard_focus_changed(s->client);
    }
}

/* Modifier keys: both keys of a pair count, and the modifier stays
 * down until both are released. */
static uint8_t *modifier_of(uint32_t key, int *side)
{
    switch (key) {
    case KEY_LEFTSHIFT:  *side = 0; return &mod_shift;
    case KEY_RIGHTSHIFT: *side = 1; return &mod_shift;
    case KEY_LEFTCTRL:   *side = 0; return &mod_ctrl;
    case KEY_RIGHTCTRL:  *side = 1; return &mod_ctrl;
    case KEY_LEFTALT:    *side = 0; return &mod_alt;
    case KEY_RIGHTALT:   *side = 1; return &mod_alt;
    case KEY_LEFTMETA:   *side = 0; return &mod_logo;
    case KEY_RIGHTMETA:  *side = 1; return &mod_logo;
    }
    return NULL;
}

void seat_key(uint32_t key, int pressed)
{
    int side;
    uint8_t *mod = modifier_of(key, &side);
    if (mod) {
        *mod = pressed ? (uint8_t)(*mod | (1u << side)) : (uint8_t)(*mod & ~(1u << side));
        modifiers = (mod_shift ? KEYMAP_MOD_SHIFT : 0) | (mod_ctrl ? KEYMAP_MOD_CTRL : 0) |
                    (mod_alt ? KEYMAP_MOD_ALT : 0) | (mod_logo ? KEYMAP_MOD_LOGO : 0);
        if (keyboard_focus && keyboard_focus->client->keyboard)
            keyboard_send_modifiers(keyboard_focus->client->keyboard, serial_for(keyboard_focus->client), (uint32_t)modifiers, 0, 0, 0);
        return;
    }
    if (pressed) {
        if ((modifiers & KEYMAP_MOD_ALT) && key == KEY_TAB) { toplevel_cycle(); return; }
        if ((modifiers & KEYMAP_MOD_ALT) && key == KEY_F4) {
            struct toplevel *t = toplevel_focused();
            if (t)
                toplevel_close(t);
            return;
        }
        /* Print Screen saves a screenshot. Alt+SysRq prints the thread
         * table of the kernel and is passed to the client unchanged. */
        if (key == KEY_SYSRQ && !(modifiers & KEYMAP_MOD_ALT)) {
            char *const argv[] = { "/bin/screenshot", NULL };
            int err = mime_spawn(argv);
            comp_log(err < 0 ? "cannot start /bin/screenshot" : "screenshot started");
            return;
        }
        if (key == KEY_ESC && popup_grab_surface()) {
            popup_dismiss_all();
            return;
        }
        if (npressed < 16)
            pressed_keys[npressed++] = key;
    } else {
        for (int i = 0; i < npressed; i++)
            if (pressed_keys[i] == key) {
                pressed_keys[i] = pressed_keys[--npressed];
                break;
            }
    }
    if (keyboard_focus && keyboard_focus->client->keyboard) {
        text_key(key, pressed, modifiers);
        keyboard_send_key(keyboard_focus->client->keyboard, serial_for(keyboard_focus->client), now_ms(), key, pressed ? 1 : 0);
        if (pressed)
            comp_debug("key 0x%02x to surface %d", key, keyboard_focus->id);
    }
}

void seat_repeat_changed(void)
{
    for (struct wire_client *c = wire_server_first_client(server_of_seat); c; c = wire_client_next(c)) {
        struct client *cl = wire_client_get_user_data(c);
        if (cl && cl->keyboard)
            keyboard_send_repeat_info(cl->keyboard, settings.repeat_rate, settings.repeat_delay);
    }
}

void seat_surface_gone(struct csurface *s)
{
    if (cursor_surface == s) {
        scene_cursor_changed();
        cursor_surface = NULL;
        cursor_is_hidden = 0;
        scene_cursor_changed();
    }
    if (pointer_focus == s)
        pointer_focus = NULL;
    if (press_focus == s)
        press_focus = NULL;
    if (keyboard_focus == s)
        keyboard_focus = NULL;
    text_surface_gone(s);
    if (grab_origin == s) {
        grab_origin = NULL;
        grab_client = NULL;
        grab_serial = 0;
    }
}
