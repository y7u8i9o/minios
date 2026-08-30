#pragma once
/* Unix domain stream sockets with descriptor passing (M23). */
#include <kernel.h>
#include <fs/vfs.h>

int socket_create(int flags, struct file **out);
int socket_pair(int flags, struct file **a, struct file **b);
int socket_bind(struct file *f, const char *name);
int socket_listen(struct file *f, int backlog);
int socket_accept(struct file *f, int flags, struct file **out);
int socket_connect(struct file *f, const char *name);
int socket_shutdown(struct file *f, int how);
/* Transfer data and, on send, the referenced files (references are
 * consumed on success). On receive *files gets referenced files the
 * caller installs; *nfiles is the room available and receives the
 * count delivered. */
long socket_send(struct file *f, const char *buf, size_t n, struct file **files, int nfiles);
long socket_recv(struct file *f, char *buf, size_t n, struct file **files, int *nfiles);
bool file_is_socket(const struct file *f);
