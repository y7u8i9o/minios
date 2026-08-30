#pragma once
/* Unix domain stream sockets with descriptor passing (M23). */
#include <sys/types.h>
#include <minios/abi.h>

typedef uint32_t socklen_t;
struct sockaddr { uint16_t sa_family; char sa_data[14]; };

int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int fds[2]);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
int accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
int shutdown(int fd, int how);
ssize_t sendmsg(int fd, const struct msghdr *msg, int flags);
ssize_t recvmsg(int fd, struct msghdr *msg, int flags);
ssize_t send(int fd, const void *buf, size_t n, int flags);
ssize_t recv(int fd, void *buf, size_t n, int flags);
