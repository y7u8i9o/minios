#pragma once
/* Server side: a listening socket, clients with resources, globals. */
#include <wire/common.h>

struct wire_server;
struct wire_client;
struct wire_resource;

typedef void (*wire_bind_fn)(struct wire_client *client, void *data, uint32_t version, uint32_t id);
typedef void (*wire_resource_destroy_fn)(struct wire_resource *r);
typedef void (*wire_client_destroy_fn)(struct wire_client *c, void *data);

struct wire_resource {
    struct wire_object obj;
    struct wire_client *client;
    const void *listener;       /* function pointers in request order */
    void *data;
    wire_resource_destroy_fn destroy;
    struct wire_resource *next; /* client's list */
};

struct wire_global {
    uint32_t name;
    const struct wire_interface *iface;
    int version;
    wire_bind_fn bind;
    void *data;
    struct wire_global *next;
};

struct wire_server *wire_server_create(const char *name);   /* NULL name: "display" */
void wire_server_destroy(struct wire_server *s);
int wire_server_fd(struct wire_server *s);
/* Accept a pending connection; returns the new client. */
struct wire_client *wire_server_accept(struct wire_server *s);
/* Wrap an already connected descriptor (tests). */
struct wire_client *wire_server_add_client(struct wire_server *s, int fd);
struct wire_client *wire_server_first_client(struct wire_server *s);
struct wire_client *wire_client_next(struct wire_client *c);
uint32_t wire_server_next_serial(struct wire_server *s);

struct wire_global *wire_global_create(struct wire_server *s, const struct wire_interface *iface, int version,
                                       wire_bind_fn bind, void *data);
void wire_global_destroy(struct wire_server *s, struct wire_global *g);

int wire_client_fd(struct wire_client *c);
/* Read once and dispatch; returns -1 when the client is gone (the
 * caller then destroys it). */
int wire_client_dispatch(struct wire_client *c);
int wire_client_flush(struct wire_client *c);
size_t wire_client_pending(struct wire_client *c);   /* bytes not yet sent */
void wire_client_destroy(struct wire_client *c);
void wire_client_set_user_data(struct wire_client *c, void *data, wire_client_destroy_fn destroy);
void *wire_client_get_user_data(struct wire_client *c);
struct wire_server *wire_client_server(struct wire_client *c);
struct wire_resource *wire_client_find(struct wire_client *c, uint32_t id);
struct wire_resource *wire_client_first_resource(struct wire_client *c);   /* iterate with ->next */
void wire_client_post_error(struct wire_client *c, struct wire_resource *r, uint32_t code, const char *message);

struct wire_resource *wire_resource_create(struct wire_client *c, const struct wire_interface *iface, int version, uint32_t id);
void wire_resource_set_listener(struct wire_resource *r, const void *listener, void *data, wire_resource_destroy_fn destroy);
void wire_resource_post(struct wire_resource *r, uint32_t opcode, const union wire_arg *args);
void wire_resource_destroy(struct wire_resource *r);
static inline uint32_t wire_resource_id(const struct wire_resource *r) { return r->obj.id; }
