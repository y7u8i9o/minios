#pragma once
/* The common socket layer (N01, docs/design/network.md): one object per
 * socket with a family, a type, a protocol, a backend operation table,
 * the pending asynchronous error and one poll source whose identity never
 * changes. Descriptor installation, file references, address copies and
 * flag validation happen here; backends receive kernel copies only. Unix
 * domain stream sockets (M23, ipc/unix_socket.c) are the first backend,
 * the Internet family (net/inet_socket.c) registers its protocols in
 * later milestones. */
#include <kernel.h>
#include <fs/vfs.h>
#include <ipc/poll.h>
#include <sync/spinlock.h>

struct socket;

/* A message crossing the layer. data is a kernel buffer holding the bytes
 * to send or the room to receive into. addr is the destination or, on
 * receive, receives the source; addrlen is the bytes valid or the bytes
 * filled, 0 when there is none. files are the SCM_RIGHTS references of
 * Unix sockets: on send they are consumed on success, on receive nfiles
 * is the room and receives the count delivered. flags are the validated
 * MSG_* bits; rflags receives MSG_TRUNC and MSG_CTRUNC. */
struct socket_msg {
    char *data;
    size_t len;
    struct sockaddr_storage *addr;
    socklen_t addrlen;
    struct file **files;
    int nfiles;
    int flags;
    int rflags;
};

/* Backend operations. Every one may be NULL: the common layer answers
 * EOPNOTSUPP for it. accept returns a new socket of the same family that
 * the common layer wraps in a file. getname fills the local or the peer
 * address; *len is the room offered and receives the full length. */
struct socket_ops {
    int (*bind)(struct socket *s, const struct sockaddr_storage *addr, socklen_t len);
    int (*listen)(struct socket *s, int backlog);
    int (*accept)(struct socket *s, struct socket **out);
    int (*connect)(struct socket *s, const struct sockaddr_storage *addr, socklen_t len);
    int (*shutdown)(struct socket *s, int how);
    long (*sendmsg)(struct socket *s, struct socket_msg *m);
    long (*recvmsg)(struct socket *s, struct socket_msg *m);
    int (*poll)(struct socket *s);
    int (*getname)(struct socket *s, struct sockaddr_storage *addr, socklen_t *len, bool peer);
    int (*setsockopt)(struct socket *s, int level, int name, const void *val, socklen_t len);
    int (*getsockopt)(struct socket *s, int level, int name, void *val, socklen_t *len);
    void (*release)(struct socket *s);
};

/* One socket. lock protects error and is never held across a backend
 * operation. poll is the sole source pollers register on; the backend
 * notifies it after every readiness change and it stays valid until the
 * socket is freed. file is the open file description, set once when the
 * descriptor is created, and is what carries O_NONBLOCK. */
struct socket {
    int family;
    int type;
    int protocol;
    const struct socket_ops *ops;
    struct spinlock lock;
    int error;
    struct poll_source poll;
    struct file *file;
    void *priv;
};

/* A family: validates the type and protocol of a new socket and installs
 * the backend (ops and priv) with create. Registered at boot. */
struct socket_family {
    int family;
    int (*create)(struct socket *s);
};

void socket_init(void);
int socket_register_family(const struct socket_family *fam);

/* Allocate a socket with its lock and poll source; backends use it for
 * accepted connections. Freed by socket_free after the backend released. */
struct socket *socket_alloc(int family, int type, int protocol, const struct socket_ops *ops);
void socket_free(struct socket *s);
/* Wrap s in a file (consuming s on failure). fflags carries O_NONBLOCK. */
int socket_file(struct socket *s, int fflags, struct file **out);

/* Create a socket with the family's backend and a file for it. */
int socket_create(int family, int type, int protocol, int fflags, struct file **out);
/* A connected pair; AF_UNIX only. */
int socket_pair(int family, int type, int protocol, int fflags, struct file **a, struct file **b);
/* The socket behind f, or NULL if f is not a socket. */
struct socket *socket_from_file(struct file *f);
bool file_is_socket(const struct file *f);

int socket_bind(struct socket *s, const struct sockaddr_storage *addr, socklen_t len);
int socket_listen(struct socket *s, int backlog);
/* Accept a connection into a new file; peer, when given, receives the
 * peer address and *peerlen its full length (room in, length out). */
int socket_accept(struct socket *s, int fflags, struct file **out,
                  struct sockaddr_storage *peer, socklen_t *peerlen);
int socket_connect(struct socket *s, const struct sockaddr_storage *addr, socklen_t len);
int socket_shutdown(struct socket *s, int how);
/* Send or receive with validated flags; unknown bits give EINVAL and a
 * backend rejects what it does not implement with EOPNOTSUPP. */
long socket_sendmsg(struct socket *s, struct socket_msg *m);
long socket_recvmsg(struct socket *s, struct socket_msg *m);
int socket_getname(struct socket *s, struct sockaddr_storage *addr, socklen_t *len, bool peer);
int socket_setsockopt(struct socket *s, int level, int name, const void *val, socklen_t len);
int socket_getsockopt(struct socket *s, int level, int name, void *val, socklen_t *len);
/* Record an asynchronous error, reported once by SO_ERROR. */
void socket_set_error(struct socket *s, int err);
/* Take the pending error, leaving none. */
int socket_take_error(struct socket *s);
bool socket_nonblocking(const struct socket *s);
/* The MSG_* bits the common layer accepts at all. */
#define SOCKET_MSG_FLAGS (MSG_OOB | MSG_PEEK | MSG_TRUNC | MSG_DONTWAIT | MSG_EOR | MSG_WAITALL | MSG_NOSIGNAL)
/* The largest option value the common layer copies. */
#define SOCKET_OPT_MAX 64

/* Bounded address copies (docs/design/network.md). from_user checks the
 * length against the family minimum and the storage size; to_user writes
 * min(room, len) bytes and stores the full length in *ulenp. */
int socket_addr_from_user(uintptr_t uaddr, size_t ulen, struct sockaddr_storage *out, socklen_t *outlen);
int socket_addr_to_user(uintptr_t uaddr, uintptr_t ulenp, const struct sockaddr_storage *addr, socklen_t len);
/* The smallest valid length of an address of the family, 0 if unknown. */
socklen_t socket_addr_min_len(int family);

/* The Unix backend (ipc/unix_socket.c). */
int unix_socket_create(struct socket *s);
int unix_socket_pair(struct socket *a, struct socket *b);
