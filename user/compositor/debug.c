/* Debugging and settings globals: statistics, the surface list, a
 * pixel probe, and tunable settings (frame interval, desktop colour,
 * key repeat, decoration default, logging). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "comp.h"

struct comp_settings settings = { FRAME_MS, 0x00306080, 30, 500, DECOR_SERVER, 0, 0 };
static struct wire_server *server;

static const char *role_name(enum role r)
{
    switch (r) {
    case ROLE_TOPLEVEL: return "toplevel";
    case ROLE_POPUP: return "popup";
    case ROLE_LAYER: return "layer";
    case ROLE_CURSOR: return "cursor";
    case ROLE_DND_ICON: return "dnd-icon";
    default: return "none";
    }
}

static void h_get_stats(struct wire_client *c, struct wire_resource *self)
{
    long count, ms, max;
    scene_stat_values(&count, &ms, &max);
    uint32_t clients = 0, surfaces = 0;
    for (struct wire_client *k = wire_server_first_client(server); k; k = wire_client_next(k))
        clients++;
    for (struct csurface *s = surface_first(); s; s = s->next)
        surfaces++;
    debug_send_stats(self, (uint32_t)uptime_ms(), (uint32_t)count, (uint32_t)ms, (uint32_t)max, clients, surfaces,
                     (uint32_t)settings.frame_ms);
}

static void h_get_surfaces(struct wire_client *c, struct wire_resource *self)
{
    for (struct csurface *s = surface_first(); s; s = s->next) {
        const char *title = s->role == ROLE_TOPLEVEL && s->toplevel ? s->toplevel->title : "";
        debug_send_surface(self, (uint32_t)s->id, (uint32_t)(s->client ? s->client->number : 0), role_name(s->role), title,
                           s->x, s->y, s->width, s->height, (uint32_t)s->mapped,
                           s->current.buffer ? s->current.buffer->format : 0);
    }
    debug_send_surfaces_done(self);
}

static void h_read_pixel(struct wire_client *c, struct wire_resource *self, int32_t x, int32_t y)
{
    uint32_t v = 0;
    if (x >= 0 && y >= 0 && x < screen_w && y < screen_h)
        v = back.pixels[(size_t)(y * screen_scale) * back.stride + (size_t)(x * screen_scale)];
    debug_send_pixel(self, x, y, v);
}
static const struct debug_impl debug_handlers = { h_get_stats, h_get_surfaces, h_read_pixel };

static void bind_debug(struct wire_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wire_resource *r = wire_resource_create(c, &debug_interface, (int)version, id);
    if (r)
        wire_resource_set_listener(r, &debug_handlers, NULL, NULL);
}

/* ---- settings ---- */

static const char *const keys[] = { "frame_ms", "desktop_color", "repeat_rate", "repeat_delay", "decorations", "verbose",
                                    "display_mode" };

static int *slot(const char *key)
{
    if (strcmp(key, "frame_ms") == 0) return &settings.frame_ms;
    if (strcmp(key, "desktop_color") == 0) return &settings.desktop_color;
    if (strcmp(key, "repeat_rate") == 0) return &settings.repeat_rate;
    if (strcmp(key, "repeat_delay") == 0) return &settings.repeat_delay;
    if (strcmp(key, "decorations") == 0) return &settings.decor_default;
    if (strcmp(key, "verbose") == 0) return &settings.verbose;
    if (strcmp(key, "display_mode") == 0) return &settings.display_mode;
    return NULL;
}

static void h_set(struct wire_client *c, struct wire_resource *self, const char *key, int32_t value)
{
    int *p = slot(key);
    if (!p)
        return;
    if (p == &settings.frame_ms && (value < 4 || value > 200)) return;
    if (p == &settings.repeat_rate && (value < 1 || value > 100)) return;
    if (p == &settings.repeat_delay && (value < 50 || value > 2000)) return;
    if (p == &settings.decor_default && value != DECOR_SERVER && value != DECOR_CLIENT) return;
    if (p == &settings.display_mode) {
        if (value == settings.display_mode)
            return;
        if (comp_set_mode(DISPLAY_MODE_W(value), DISPLAY_MODE_H(value), DISPLAY_MODE_S(value)) < 0) {
            comp_log("setting display_mode %dx%d@%d refused", DISPLAY_MODE_W(value), DISPLAY_MODE_H(value),
                     DISPLAY_MODE_S(value));
            return;
        }
        value = settings.display_mode;      /* what the backend reports */
    }
    *p = value;
    comp_log("setting %s = %d", key, value);
    if (p == &settings.frame_ms)
        frame_clock_set(settings.frame_ms);
    if (p == &settings.desktop_color)
        scene_damage_all();
    if (p == &settings.repeat_rate || p == &settings.repeat_delay)
        seat_repeat_changed();
    /* Every settings client sees the change. */
    for (struct wire_client *k = wire_server_first_client(server); k; k = wire_client_next(k))
        for (struct wire_resource *r = wire_client_first_resource(k); r; r = r->next)
            if (r->obj.interface == &settings_interface)
                settings_send_value(r, key, value);
}

static void h_get_all(struct wire_client *c, struct wire_resource *self)
{
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        settings_send_value(self, keys[i], *slot(keys[i]));
    settings_send_done(self);
}
static const struct settings_impl settings_handlers = { h_set, h_get_all };

static void bind_settings(struct wire_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wire_resource *r = wire_resource_create(c, &settings_interface, (int)version, id);
    if (r)
        wire_resource_set_listener(r, &settings_handlers, NULL, NULL);
}

void debug_init(struct wire_server *srv)
{
    server = srv;
    wire_global_create(srv, &debug_interface, 1, bind_debug, NULL);
    wire_global_create(srv, &settings_interface, 1, bind_settings, NULL);
}
