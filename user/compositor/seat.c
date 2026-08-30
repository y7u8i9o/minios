/* The seat: pointer focus and events, keyboard focus and events with
 * the keymap descriptor, modifiers, serials, cursor surfaces, and the
 * compositor's own shortcuts (Alt+Tab, Alt+F4, Alt drag). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "comp.h"

int decor_hit(const struct csurface *s, int x, int y);

static struct csurface *pointer_focus, *keyboard_focus, *press_focus;
static int buttons, modifiers;
static uint32_t last_serial;
static struct wire_server *server_of_seat;
static int keymap_fd = -1;
static uint32_t keymap_size;
static uint32_t pressed_keys[16];
static int npressed;

uint32_t seat_last_serial(void) { return last_serial; }
int seat_modifiers(void) { return modifiers; }
struct csurface *seat_keyboard_focus(void) { return keyboard_focus; }

static uint32_t serial(void)
{
    last_serial = comp_serial();
    return last_serial;
}

static uint32_t now_ms(void) { return (uint32_t)uptime_ms(); }

/* ---- resources ---- */

static void h_set_cursor(struct wire_client *c, struct wire_resource *self, uint32_t serial_, struct wire_resource *surface, int32_t hx, int32_t hy)
{
    if (!surface)
        return;
    struct csurface *s = surface->data;
    if (s->role == ROLE_NONE) {
        s->role = ROLE_CURSOR;
        comp_log("cursor surface %d", s->id);
    }
    s->hotspot_x = hx;
    s->hotspot_y = hy;
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
    keyboard_send_enter(cl->keyboard, serial(), s->res, &keys);
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
    if (pointer_focus && pointer_focus->client == cl)
        pointer_send_enter(r, serial(), pointer_focus->res, wire_fixed_from_int(cursor_x - pointer_focus->x),
                           wire_fixed_from_int(cursor_y - pointer_focus->y));
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
void seat_init(struct wire_server *srv)
{
    int fd = open("/usr/share/keymaps/us.mkm", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char buf[4096];
        ssize_t n = read(fd, buf, sizeof buf);
        close(fd);
        if (n > 0) {
            keymap_fd = memfd_create("keymap", MFD_CLOEXEC);
            if (keymap_fd >= 0 && ftruncate(keymap_fd, 4096) == 0) {
                void *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, keymap_fd, 0);
                if (map != MAP_FAILED) {
                    memcpy(map, buf, (size_t)n);
                    munmap(map, 4096);
                    keymap_size = (uint32_t)n;
                }
            }
        }
    }
    server_of_seat = srv;
    wire_global_create(srv, &seat_interface, 1, bind_seat, NULL);
}

/* ---- pointer ---- */

static void set_pointer_focus(struct csurface *s)
{
    if (pointer_focus == s)
        return;
    if (pointer_focus && pointer_focus->client->pointer) {
        pointer_send_leave(pointer_focus->client->pointer, serial(), pointer_focus->res);
        pointer_send_frame(pointer_focus->client->pointer);
    }
    pointer_focus = s;
    if (s && s->client->pointer) {
        pointer_send_enter(s->client->pointer, serial(), s->res, wire_fixed_from_int(cursor_x - s->x),
                           wire_fixed_from_int(cursor_y - s->y));
        pointer_send_frame(s->client->pointer);
        comp_log("pointer enter surface %d", s->id);
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
        pointer_send_motion(target->client->pointer, now_ms(), wire_fixed_from_int(cursor_x - target->x),
                            wire_fixed_from_int(cursor_y - target->y));
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
        struct csurface *grab = popup_grab_surface();
        struct csurface *target = scene_surface_at(cursor_x, cursor_y);
        if (grab && (!target || target->role != ROLE_POPUP)) {
            popup_dismiss_all();
            return;
        }
        /* Decorations of the toplevel under the cursor. */
        for (struct csurface *s = surface_first(); s; s = s->next)
            if (decor_hit(s, cursor_x, cursor_y) && !s->toplevel->minimized && (!target || target->stack <= s->stack)) {
                toplevel_activate(s->toplevel);
                decor_press(s, button);
                return;
            }
        if (target) {
            if (target->role == ROLE_TOPLEVEL)
                toplevel_activate(target->toplevel);
            else if (target->role == ROLE_LAYER && target->layer->interactive)
                seat_set_keyboard_focus(target);
            press_focus = target;
            if (button == 1 && (modifiers & 4) && target->role == ROLE_TOPLEVEL) {
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
        if (decor_release())
            return;
        if (!buttons)
            press_focus = NULL;
    }
    struct csurface *t = pointer_focus;
    if (t && t->client->pointer) {
        pointer_send_button(t->client->pointer, serial(), now_ms(), (uint32_t)button, pressed ? 1 : 0);
        pointer_send_frame(t->client->pointer);
        comp_log("button %d %s in surface %d", button, pressed ? "down" : "up", t->id);
    }
}

void seat_pointer_axis(int delta)
{
    struct csurface *t = scene_surface_at(cursor_x, cursor_y);
    if (t && t->client->pointer) {
        pointer_send_axis(t->client->pointer, now_ms(), 0, wire_fixed_from_int(delta * 15));
        pointer_send_frame(t->client->pointer);
        comp_log("axis %d in surface %d", delta, t->id);
    }
}

/* ---- keyboard ---- */

void seat_set_keyboard_focus(struct csurface *s)
{
    if (keyboard_focus == s)
        return;
    if (keyboard_focus && keyboard_focus->client->keyboard)
        keyboard_send_leave(keyboard_focus->client->keyboard, serial(), keyboard_focus->res);
    keyboard_focus = s;
    if (s) {
        if (s->client->keyboard)
            send_keyboard_enter(s->client, s);
        comp_log("keyboard focus surface %d", s->id);
        data_keyboard_focus_changed(s->client);
    }
}

void seat_key(uint32_t key, int pressed)
{
    int bit = key == 0x2a || key == 0x36 ? 1 : key == 0x1d ? 2 : key == 0x38 ? 4 : 0;
    if (bit) {
        modifiers = pressed ? modifiers | bit : modifiers & ~bit;
        if (keyboard_focus && keyboard_focus->client->keyboard)
            keyboard_send_modifiers(keyboard_focus->client->keyboard, serial(), (uint32_t)modifiers, 0, 0, 0);
        return;
    }
    if (pressed) {
        if ((modifiers & 4) && key == 0x0f) { toplevel_cycle(); return; }
        if ((modifiers & 4) && key == 0x3e) {
            struct toplevel *t = toplevel_focused();
            if (t)
                toplevel_close(t);
            return;
        }
        if (key == 0x01 && popup_grab_surface()) {
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
        keyboard_send_key(keyboard_focus->client->keyboard, serial(), now_ms(), key, pressed ? 1 : 0);
        if (pressed)
            comp_log("key 0x%02x to surface %d", key, keyboard_focus->id);
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
    if (pointer_focus == s)
        pointer_focus = NULL;
    if (press_focus == s)
        press_focus = NULL;
    if (keyboard_focus == s)
        keyboard_focus = NULL;
}
