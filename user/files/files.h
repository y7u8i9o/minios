#pragma once
/* Files: the window (files.c) and the file operations it runs
 * (fsops.c). Operations return 0 or -errno. */
#include <stddef.h>

/* dir + "/" + name, with a single slash after the root. */
void fs_join(char *out, size_t size, const char *dir, const char *name);
/* Collapse repeated slashes, "." and ".." components in place. */
void fs_normalize(char *path);
/* The last component of a path ("/" for the root). */
const char *fs_basename(const char *path);
int fs_copy(const char *src, const char *dst);           /* files and whole directories */
int fs_remove(const char *path);                          /* files and whole directories */
int fs_move(const char *src, const char *dst);            /* rename, else copy and remove */
/* Bytes, files and directories below a path (the path itself counted). */
long fs_tree_size(const char *path, int *files, int *dirs);
/* "512 bytes", "1.2 KB", "3.4 MB" into buf. */
const char *fs_human_size(long size, char *buf, size_t size_buf);
