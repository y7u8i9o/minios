#pragma once
/* Files: the window (files.c) and the path helpers it uses (fsops.c).
 * Copy, remove and move are in gui/fileops.h. Operations return 0 or
 * -errno. */
#include <stddef.h>

/* dir + "/" + name, with a single slash after the root. */
void fs_join(char *out, size_t size, const char *dir, const char *name);
/* Collapse repeated slashes, "." and ".." components in place. */
void fs_normalize(char *path);
/* The last component of a path ("/" for the root). */
const char *fs_basename(const char *path);
/* Bytes, files and directories below a path (the path itself counted). */
long fs_tree_size(const char *path, int *files, int *dirs);
