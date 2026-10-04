#pragma once
/* Client side: a display connection with proxies and listeners. */
#include <wire/common.h>

struct wire_display;

struct wire_proxy {
    struct wire_object obj;
    struct wire_display *display;
    const void *listener;       /* function pointers in event order */
    void *data;
    int destroyed;
};

/* Connect to the compositor's abstract socket name ("display" when
 * NULL) or wrap an existing descriptor. */
struct wire_display *wire_display_connect(const char *name);
struct wire_display *wire_display_connect_fd(int fd);
void wire_display_disconnect(struct wire_display *d);
int wire_display_fd(struct wire_display *d);
struct wire_proxy *wire_display_proxy(struct wire_display *d);
int wire_display_flush(struct wire_display *d);
/* Read once (blocking) and dispatch every complete message. Returns the
 * number dispatched or -1 when the connection is gone. */
int wire_display_dispatch(struct wire_display *d);
/* Dispatch what is buffered without reading. */
int wire_display_dispatch_pending(struct wire_display *d);
/* sync and dispatch until its callback fires. */
int wire_display_roundtrip(struct wire_display *d);
/* Last protocol error (0 when none). */
int wire_display_error(struct wire_display *d);

struct wire_proxy *wire_proxy_create(struct wire_proxy *factory, const struct wire_interface *iface, int version);
void wire_proxy_marshal(struct wire_proxy *p, uint32_t opcode, const union wire_arg *args, struct wire_proxy *created);
void wire_proxy_destroy(struct wire_proxy *p);
int wire_proxy_add_listener(struct wire_proxy *p, const void *listener, void *data);
void wire_proxy_set_user_data(struct wire_proxy *p, void *data);
void *wire_proxy_get_user_data(struct wire_proxy *p);
static inline uint32_t wire_proxy_id(const struct wire_proxy *p) { return p->obj.id; }
