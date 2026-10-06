/* The client side of the notification protocol (gui/notify.h). Each posted
 * notification is a notification proxy. The proxy remains until notifyd
 * reports that the notification has closed, so that the actions of the
 * notification can reach the program. */
#include <gui/notify.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wire/client.h>
#include "core-client.h"
#include "notify-client.h"

#define NOTIFY_SOCKET "notify"

struct posted {
    struct notify_client *client;
    struct wire_proxy *proxy;
    uint32_t number;
    int shown;
    notify_event_fn fn;
    void *arg;
    struct posted *next;
};

struct notify_client {
    struct wire_display *display;
    struct wire_proxy *registry, *manager;
    struct posted *posted;
};

void notify_spec_init(struct notify_spec *s, const char *app_name, const char *summary, const char *body)
{
    memset(s, 0, sizeof *s);
    s->app_name = app_name;
    s->summary = summary;
    s->body = body;
    s->urgency = NOTIFY_NORMAL;
    s->timeout_ms = -1;
}

static void on_global(void *data, struct wire_proxy *registry, uint32_t name, const char *iface, uint32_t version)
{
    struct notify_client *c = data;
    if (strcmp(iface, "notify_manager") == 0 && !c->manager)
        c->manager = registry_bind(registry, name, iface, version, &notify_manager_interface, 1);
}

static void on_global_remove(void *data, struct wire_proxy *registry, uint32_t name)
{
}

static const struct registry_listener registry_events = { on_global, on_global_remove };

struct notify_client *notify_client_connect(void)
{
    struct notify_client *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->display = wire_display_connect(NOTIFY_SOCKET);
    if (!c->display) {
        free(c);
        return NULL;
    }
    c->registry = display_get_registry(wire_display_proxy(c->display));
    if (!c->registry || registry_add_listener(c->registry, &registry_events, c) < 0 ||
        wire_display_roundtrip(c->display) < 0 || !c->manager) {
        notify_client_disconnect(c);
        return NULL;
    }
    return c;
}

void notify_client_disconnect(struct notify_client *c)
{
    if (!c)
        return;
    while (c->posted) {
        struct posted *p = c->posted;
        c->posted = p->next;
        free(p);
    }
    wire_display_disconnect(c->display);
    free(c);
}

int notify_client_fd(struct notify_client *c)
{
    return wire_display_fd(c->display);
}

static void forget(struct posted *p)
{
    struct notify_client *c = p->client;
    for (struct posted **q = &c->posted; *q; q = &(*q)->next)
        if (*q == p) {
            *q = p->next;
            break;
        }
    notification_destroy(p->proxy);
    free(p);
}

static void on_shown(void *data, struct wire_proxy *proxy, uint32_t number)
{
    struct posted *p = data;
    p->number = number;
    p->shown = 1;
}

static void on_action(void *data, struct wire_proxy *proxy, const char *key)
{
    struct posted *p = data;
    if (p->fn)
        p->fn(p->arg, p->number, key, 0);
}

static void on_closed(void *data, struct wire_proxy *proxy, uint32_t reason)
{
    struct posted *p = data;
    if (p->fn)
        p->fn(p->arg, p->number, NULL, (enum notify_closed)reason);
    forget(p);
}

static const struct notification_listener notification_events = { on_shown, on_action, on_closed };

int notify_client_dispatch(struct notify_client *c)
{
    int r = wire_display_dispatch(c->display);
    wire_display_flush(c->display);
    return r < 0 ? -1 : 0;
}

long notify_client_post(struct notify_client *c, const struct notify_spec *s, notify_event_fn fn, void *arg)
{
    if (!s->summary || s->nactions < 0 || s->nactions > 3)
        return -EINVAL;
    struct posted *p = calloc(1, sizeof *p);
    if (!p)
        return -ENOMEM;
    p->client = c;
    p->fn = fn;
    p->arg = arg;
    p->proxy = notify_manager_create(c->manager, s->app_name ? s->app_name : "", s->summary,
                                     s->body ? s->body : "");
    if (!p->proxy) {
        free(p);
        return -ENOMEM;
    }
    notification_add_listener(p->proxy, &notification_events, p);
    p->next = c->posted;
    c->posted = p;
    if (s->icon)
        notification_set_icon(p->proxy, s->icon);
    if (s->urgency != NOTIFY_NORMAL)
        notification_set_urgency(p->proxy, s->urgency);
    if (s->timeout_ms >= 0)
        notification_set_timeout(p->proxy, s->timeout_ms);
    for (int i = 0; i < s->nactions; i++)
        notification_add_action(p->proxy, s->actions[i].key, s->actions[i].label ? s->actions[i].label : "");
    if (s->replaces)
        notification_set_replaces(p->proxy, s->replaces);
    notification_show(p->proxy);
    /* The roundtrip delivers the shown event, which carries the number of
     * the notification. */
    if (wire_display_roundtrip(c->display) < 0 || wire_display_error(c->display))
        return -EIO;
    for (struct posted *q = c->posted; q; q = q->next)
        if (q == p)
            return p->shown ? (long)p->number : -EIO;
    return -EIO;                        /* the notification closed at once */
}

int notify_client_close(struct notify_client *c, uint32_t number)
{
    for (struct posted *p = c->posted; p; p = p->next)
        if (p->number == number) {
            notification_close(p->proxy);
            wire_display_flush(c->display);
            return 0;
        }
    return -ENOENT;
}

long gui_notify(const struct notify_spec *s)
{
    struct notify_client *c = notify_client_connect();
    if (!c)
        return -ENOENT;
    long number = notify_client_post(c, s, NULL, NULL);
    notify_client_disconnect(c);
    return number;
}
