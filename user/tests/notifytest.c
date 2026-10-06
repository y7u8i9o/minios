/* The notification protocol (docs/design/notifications.md). notifytest
 * starts notifyd and acts in both roles that notifyd serves. As a client,
 * it posts notifications through the libgui API. As a display, it watches
 * and operates the notifications through a notify_display, as the panel
 * does. It checks the numbers, replacement, closing by the sender, expiry,
 * actions, dismissal, removal, clearing, the do-not-disturb state, the
 * history limit and the initial state that a new display receives. */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <gui/notify.h>
#include <wire/client.h>
#include "core-client.h"
#include "notify-client.h"

static int failures;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            failures++;                                                                                               \
            printf("notifytest: FAIL " __VA_ARGS__);                                                                  \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)

/* ---- the display side ---- */

struct view {
    struct wire_display *d;
    struct wire_proxy *manager, *display;
    int notifications, popups_shown, popups_hidden, removed, dnd;
    uint32_t last_number, last_popup, last_removed;
    char last_summary[128], last_actions[256];
};

static void on_notification(void *user, struct wire_proxy *p, uint32_t number, const char *app, const char *summary,
                            const char *body, const char *icon, uint32_t urgency, uint32_t time, const char *actions)
{
    struct view *v = user;
    v->notifications++;
    v->last_number = number;
    strlcpy(v->last_summary, summary, sizeof v->last_summary);
    strlcpy(v->last_actions, actions, sizeof v->last_actions);
}

static void on_popup(void *user, struct wire_proxy *p, uint32_t number, uint32_t visible)
{
    struct view *v = user;
    if (visible)
        v->popups_shown++;
    else
        v->popups_hidden++;
    v->last_popup = number;
}

static void on_removed(void *user, struct wire_proxy *p, uint32_t number)
{
    struct view *v = user;
    v->removed++;
    v->last_removed = number;
}

static void on_dnd(void *user, struct wire_proxy *p, uint32_t on)
{
    struct view *v = user;
    v->dnd = (int)on;
}

static const struct notify_display_listener display_events = { on_notification, on_popup, on_removed, on_dnd };

static void on_global(void *user, struct wire_proxy *registry, uint32_t name, const char *iface, uint32_t version)
{
    struct view *v = user;
    if (strcmp(iface, "notify_manager") == 0)
        v->manager = registry_bind(registry, name, iface, version, &notify_manager_interface, 1);
}

static void on_global_remove(void *user, struct wire_proxy *registry, uint32_t name)
{
}

static const struct registry_listener registry_events = { on_global, on_global_remove };

static int view_open(struct view *v)
{
    memset(v, 0, sizeof *v);
    v->d = wire_display_connect("notify");
    if (!v->d)
        return -1;
    struct wire_proxy *registry = display_get_registry(wire_display_proxy(v->d));
    registry_add_listener(registry, &registry_events, v);
    if (wire_display_roundtrip(v->d) < 0 || !v->manager)
        return -1;
    v->display = notify_manager_get_display(v->manager);
    notify_display_add_listener(v->display, &display_events, v);
    return wire_display_roundtrip(v->d) < 0 ? -1 : 0;
}

/* ---- the client side ---- */

struct sent {
    uint32_t number;
    int actions, closed;
    unsigned reason;
    char key[32];
};

static void on_event(void *arg, uint32_t number, const char *key, enum notify_closed reason)
{
    struct sent *s = arg;
    if (key) {
        s->actions++;
        strlcpy(s->key, key, sizeof s->key);
    } else {
        s->closed++;
        s->reason = reason;
    }
}

static long post(struct notify_client *c, struct sent *s, const char *summary, int urgency, int timeout,
                 uint32_t replaces, const struct notify_action *actions, int nactions)
{
    struct notify_spec spec;
    notify_spec_init(&spec, "notifytest", summary, "body text");
    spec.urgency = (enum notify_urgency)urgency;
    spec.timeout_ms = timeout;
    spec.replaces = replaces;
    spec.actions = actions;
    spec.nactions = nactions;
    memset(s, 0, sizeof *s);
    long number = notify_client_post(c, &spec, on_event, s);
    if (number > 0)
        s->number = (uint32_t)number;
    return number;
}

/* Handle the input of both connections for ms milliseconds. */
static void pump(struct notify_client *c, struct view *v, int ms)
{
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= ms)
            break;
        wire_display_flush(v->d);
        struct pollfd pf[2] = { { notify_client_fd(c), POLLIN, 0 }, { wire_display_fd(v->d), POLLIN, 0 } };
        if (poll(pf, 2, (int)(ms - elapsed)) <= 0)
            continue;
        if (pf[0].revents)
            notify_client_dispatch(c);
        if (pf[1].revents)
            wire_display_dispatch(v->d);
    }
}

static struct notify_client *connect_retry(void)
{
    for (int i = 0; i < 100; i++) {
        struct notify_client *c = notify_client_connect();
        if (c)
            return c;
        usleep(20000);
    }
    return NULL;
}

int main(void)
{
    pid_t daemon = fork();
    if (daemon == 0) {
        execl("/bin/notifyd", "notifyd", (char *)NULL);
        _exit(127);
    }
    CHECK(daemon > 0, "fork notifyd");
    struct notify_client *c = connect_retry();
    struct view v;
    CHECK(c != NULL, "connect as a client");
    CHECK(c && view_open(&v) == 0, "connect as a display");
    if (!c || !v.display) {
        kill(daemon, SIGTERM);
        printf("notifytest: %d failures\n", failures + 1);
        return 1;
    }

    /* A notification gets a number, appears on the display and shows a
     * pop-up. */
    struct sent a, b, x;
    long n1 = post(c, &a, "first", NOTIFY_NORMAL, 0, 0, NULL, 0);
    pump(c, &v, 100);
    CHECK(n1 == 1, "first number %ld", n1);
    CHECK(v.notifications == 1 && v.last_number == 1 && strcmp(v.last_summary, "first") == 0,
          "display saw %d notifications, last %u '%s'", v.notifications, v.last_number, v.last_summary);
    CHECK(v.popups_shown == 1 && v.last_popup == 1, "pop-up shown %d", v.popups_shown);

    /* A replacement retains the number and tells the previous sender that
     * its notification closed. */
    long n2 = post(c, &b, "second", NOTIFY_NORMAL, 0, 1, NULL, 0);
    pump(c, &v, 100);
    CHECK(n2 == 1, "replacement number %ld", n2);
    CHECK(v.notifications == 2 && v.last_number == 1 && strcmp(v.last_summary, "second") == 0,
          "replacement on the display: %d, '%s'", v.notifications, v.last_summary);
    CHECK(a.closed == 1 && a.reason == NOTIFY_CLOSED, "replaced sender closed %d reason %u", a.closed, a.reason);
    CHECK(v.popups_shown == 1, "the replaced pop-up remains a single pop-up (%d)", v.popups_shown);
    printf("notifytest: replacement ok\n");

    /* When the sender closes its notification, the notification leaves the
     * history. */
    CHECK(notify_client_close(c, 1) == 0, "close");
    pump(c, &v, 100);
    CHECK(v.removed == 1 && v.last_removed == 1, "removed %d", v.removed);
    CHECK(b.closed == 1 && b.reason == NOTIFY_CLOSED, "closed by the sender: %d reason %u", b.closed, b.reason);
    printf("notifytest: close ok\n");

    /* A short timeout expires the pop-up. The notification remains in the
     * history. */
    long n3 = post(c, &x, "expiring", NOTIFY_NORMAL, 300, 0, NULL, 0);
    pump(c, &v, 100);
    CHECK(n3 == 2 && x.closed == 0, "not yet expired: number %ld closed %d", n3, x.closed);
    pump(c, &v, 600);
    CHECK(x.closed == 1 && x.reason == NOTIFY_EXPIRED, "expired %d reason %u", x.closed, x.reason);
    CHECK(v.popups_hidden >= 1 && v.last_popup == 2 && v.removed == 1, "hidden %d, removed %d", v.popups_hidden,
          v.removed);
    printf("notifytest: expiry ok\n");

    /* An action reaches the sender, and the pop-up closes. */
    static const struct notify_action actions[] = { { "default", "" }, { "reply", "Reply" } };
    long n4 = post(c, &a, "with actions", NOTIFY_NORMAL, 0, 0, actions, 2);
    pump(c, &v, 100);
    CHECK(strcmp(v.last_actions, "default\n\nreply\nReply\n") == 0, "actions '%s'", v.last_actions);
    notify_display_invoke(v.display, (uint32_t)n4, "reply");
    notify_display_invoke(v.display, (uint32_t)n4, "unknown");
    pump(c, &v, 100);
    CHECK(a.actions == 1 && strcmp(a.key, "reply") == 0, "action %d '%s'", a.actions, a.key);
    CHECK(a.closed == 1 && a.reason == NOTIFY_DISMISSED, "closed after the action: %d reason %u", a.closed, a.reason);
    printf("notifytest: action ok\n");

    /* Dismissal closes the pop-up. Removal from the history tells the
     * sender of a notification that is still open. */
    long n5 = post(c, &b, "dismissed", NOTIFY_NORMAL, 0, 0, NULL, 0);
    pump(c, &v, 100);
    notify_display_dismiss(v.display, (uint32_t)n5);
    pump(c, &v, 100);
    CHECK(b.closed == 1 && b.reason == NOTIFY_DISMISSED, "dismissed %d reason %u", b.closed, b.reason);
    long n6 = post(c, &x, "removed", NOTIFY_NORMAL, 0, 0, NULL, 0);
    pump(c, &v, 100);
    int removed = v.removed;
    notify_display_remove(v.display, (uint32_t)n6);
    pump(c, &v, 100);
    CHECK(v.removed == removed + 1 && v.last_removed == (uint32_t)n6, "removed from the history");
    CHECK(x.closed == 1 && x.reason == NOTIFY_REMOVED, "removal closed %d reason %u", x.closed, x.reason);
    printf("notifytest: dismiss and remove ok\n");

    /* Do-not-disturb: normal notifications enter the history without a
     * pop-up, and critical notifications still show one. */
    notify_display_set_dnd(v.display, 1);
    pump(c, &v, 100);
    CHECK(v.dnd == 1, "do not disturb reported");
    int shown = v.popups_shown, notes = v.notifications;
    post(c, &a, "quiet", NOTIFY_NORMAL, 0, 0, NULL, 0);
    pump(c, &v, 100);
    CHECK(v.notifications == notes + 1 && v.popups_shown == shown, "a quiet notification: %d pop-ups",
          v.popups_shown - shown);
    post(c, &b, "urgent", NOTIFY_CRITICAL, 0, 0, NULL, 0);
    pump(c, &v, 100);
    CHECK(v.popups_shown == shown + 1, "a critical notification shows its pop-up");
    notify_display_set_dnd(v.display, 0);
    pump(c, &v, 100);
    CHECK(v.dnd == 0, "do not disturb off");
    printf("notifytest: do not disturb ok\n");

    /* A new display receives the do-not-disturb state and the history. */
    struct view w;
    CHECK(view_open(&w) == 0, "second display");
    int history = w.notifications;
    /* Numbers 2, 3, 4, 6 and 7 remain: 1 and 5 were closed and removed. */
    CHECK(history == 5, "a new display receives %d notifications", history);
    CHECK(w.popups_shown == 1, "and %d visible pop-ups", w.popups_shown);
    wire_display_disconnect(w.d);
    printf("notifytest: initial state ok\n");

    /* The history has at most 50 entries, and the oldest entries leave
     * first. The five existing entries plus fifty new ones remove five. */
    removed = v.removed;
    for (int i = 0; i < 50; i++)
        post(c, &a, "bulk", NOTIFY_LOW, 0, 0, NULL, 0);
    for (int i = 0; i < 50 && v.removed < removed + 5; i++)
        pump(c, &v, 100);
    CHECK(v.removed == removed + 5, "history limit: %d removed", v.removed - removed);
    /* notifyd logs every removal on the serial console. The console is slow
     * under emulation, so the test waits for a generous time. */
    notify_display_clear(v.display);
    for (int i = 0; i < 50 && v.removed < removed + 55; i++)
        pump(c, &v, 100);
    CHECK(v.removed == removed + 55, "clear: %d removed", v.removed - removed);
    printf("notifytest: history ok\n");

    notify_client_disconnect(c);
    wire_display_disconnect(v.d);
    kill(daemon, SIGTERM);
    int status;
    waitpid(daemon, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "notifyd exit status 0x%x", status);
    printf("notifytest: %d failures\n", failures);
    return failures ? 1 : 0;
}
