#pragma once
/* Posting desktop notifications (src/notify.c, docs/design/notifications.md).
 *
 * Notifications go to notifyd over the "notify" protocol. A program has two
 * ways to post them. It can call gui_notify, which connects, posts and
 * disconnects. Alternatively, it can leave a notify_client open. The
 * client receives the actions that the user invokes and reports when the
 * notifications close. */
#include <stdint.h>

enum notify_urgency { NOTIFY_LOW = 0, NOTIFY_NORMAL = 1, NOTIFY_CRITICAL = 2 };

/* The reasons why a notification closes. */
enum notify_closed {
    NOTIFY_EXPIRED = 1,                 /* the pop-up timed out */
    NOTIFY_DISMISSED = 2,               /* the user closed it or invoked an action */
    NOTIFY_CLOSED = 3,                  /* the program closed it */
    NOTIFY_REMOVED = 4,                 /* the user removed it from the history */
};

struct notify_action {
    const char *key;                    /* "default" for a click on the notification */
    const char *label;
};

/* A notification. Every field except summary may be zero or NULL.
 * The value -1 for timeout_ms selects the default of notifyd (5 seconds,
 * and no expiry for critical notifications). The value 0 means that the
 * notification does not expire. The field replaces is the number of a
 * notification to replace. */
struct notify_spec {
    const char *app_name, *summary, *body, *icon;
    enum notify_urgency urgency;
    int timeout_ms;
    uint32_t replaces;
    const struct notify_action *actions;
    int nactions;                       /* at most 3 */
};

/* Set the defaults: normal urgency and the default timeout of notifyd. */
void notify_spec_init(struct notify_spec *s, const char *app_name, const char *summary, const char *body);

/* Post s and return its number. On failure, return a negative errno value,
 * which is -ENOENT if notifyd is not running. */
long gui_notify(const struct notify_spec *s);

struct notify_client;

/* Called for each action that the user invokes, with the action key in key.
 * Called once more when the notification closes, with a NULL key and the
 * reason for closing. */
typedef void (*notify_event_fn)(void *arg, uint32_t number, const char *key, enum notify_closed reason);

/* Connect to notifyd. Return NULL if notifyd is not running. */
struct notify_client *notify_client_connect(void);
void notify_client_disconnect(struct notify_client *c);
/* Return the descriptor to poll for input. The dispatch call handles the
 * input and returns -1 when the connection is gone. */
int notify_client_fd(struct notify_client *c);
int notify_client_dispatch(struct notify_client *c);
/* Post s. The function fn, which may be NULL, receives the events of the
 * notification. Return the number of the notification, or a negative errno
 * value on failure. */
long notify_client_post(struct notify_client *c, const struct notify_spec *s, notify_event_fn fn, void *arg);
/* Close a notification that this client posted. */
int notify_client_close(struct notify_client *c, uint32_t number);
