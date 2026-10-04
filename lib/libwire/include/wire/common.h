#pragma once
/* libwire: the wire format shared by clients and the compositor (M24).
 * Messages: object id (u32), opcode (u16), size (u16, header included);
 * arguments as described in protocol/core.xml. Descriptors travel in
 * SCM_RIGHTS records in argument order. */
#include <stdint.h>
#include <stddef.h>

#define WIRE_MAX_MESSAGE 65535
#define WIRE_MAX_ARGS 16
#define WIRE_MAX_FDS 32
#define WIRE_SERVER_ID_BASE 0xff000000u

struct wire_array {
    const void *data;
    size_t size;
};

union wire_arg {
    int32_t i;                  /* int, fixed (24.8) */
    uint32_t u;
    const char *s;
    const struct wire_array *a;
    uint32_t o;                 /* object id, 0 for null */
    uint32_t n;                 /* new id */
    int h;                      /* descriptor */
};

/* Signature characters: i u f s a o n h, a '?' before o/s/a allows null. */
struct wire_message {
    const char *name;
    const char *signature;
    int nargs;
    const char *const *types;   /* interface name per argument or NULL */
    int destructor;             /* the request destroys its object */
};

struct wire_interface {
    const char *name;
    int version;
    int nrequests;
    const struct wire_message *requests;
    int nevents;
    const struct wire_message *events;
};

struct wire_object {
    uint32_t id;
    const struct wire_interface *interface;
    int version;
};

/* A connection: output buffer with queued descriptors, input buffer
 * with received descriptors. */
struct wire_conn {
    int fd;
    uint8_t out[WIRE_MAX_MESSAGE + 1];
    size_t out_len;
    int out_fds[WIRE_MAX_FDS];
    int nout_fds;
    uint8_t in[2 * WIRE_MAX_MESSAGE];
    size_t in_len;
    int in_fds[WIRE_MAX_FDS];
    int nin_fds;
    int error;                  /* set once the peer is gone or the stream is corrupt */
};

void wire_conn_init(struct wire_conn *c, int fd);
void wire_conn_close(struct wire_conn *c);
/* Append a message; flushes first when the buffers would overflow. */
int wire_conn_marshal(struct wire_conn *c, uint32_t id, uint32_t opcode, const struct wire_message *m,
                      const union wire_arg *args);
int wire_conn_flush(struct wire_conn *c);
/* Read once from the socket; returns bytes read, 0 at end of file, -1 on error. */
int wire_conn_read(struct wire_conn *c);
/* Next complete message in the input buffer, without consuming it. */
int wire_conn_peek(struct wire_conn *c, uint32_t *id, uint32_t *opcode, const uint8_t **body, size_t *len);
void wire_conn_consume(struct wire_conn *c, size_t len);
/* Decode a message body; strings and arrays point into body. */
int wire_unmarshal(struct wire_conn *c, const struct wire_message *m, const uint8_t *body, size_t len,
                   union wire_arg *args, struct wire_array *arrays);
/* The encoded size of a message in bytes, its header included. */
size_t wire_message_size(const struct wire_message *m, const union wire_arg *args);
/* Write the arguments of a message into buf as text for a trace, for
 * example `buffer@15, 0, 0` or `"title", new callback@7`. Object and new
 * id arguments are ids. An untyped new id takes its interface from the
 * last string argument before it, as in registry.bind. object_name,
 * when not NULL, names the interface of an object id or returns NULL
 * for an unknown one. The text is truncated to size and always
 * terminated. Returns its length. */
typedef const char *(*wire_object_name_fn)(void *data, uint32_t id);
size_t wire_format_args(char *buf, size_t size, const struct wire_message *m, const union wire_arg *args,
                        wire_object_name_fn object_name, void *data);
/* 24.8 fixed point helpers. */
static inline int32_t wire_fixed_from_int(int v) { return v * 256; }
static inline int wire_fixed_to_int(int32_t f) { return f / 256; }
