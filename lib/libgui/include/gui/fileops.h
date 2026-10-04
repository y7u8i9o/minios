#pragma once
/* File operations shared by the file manager, the file chooser and the
 * desktop: recursive copy, remove and move, the text/uri-list format of
 * dragged files, and the drop of files into a folder (docs/design/dnd.md).
 * Errors are negative errno values. */
#include <stddef.h>

int fileops_copy(const char *src, const char *dst);     /* files and whole directories */
int fileops_remove(const char *path);                   /* files and whole directories */
int fileops_move(const char *src, const char *dst);     /* rename, else copy and remove */

/* A text/uri-list of the paths: one file URI per line, each line ending
 * in CR LF, with every byte outside the unreserved characters and the
 * slash percent encoded. The result is allocated with malloc. */
char *fileops_uri_list(const char *const *paths, int n);
/* The paths of the file URIs in a text/uri-list, decoded; comments and
 * other schemes are skipped. Returns the number of paths and stores an
 * array allocated with malloc, freed with fileops_free_paths. */
int fileops_parse_uri_list(const char *text, size_t len, char ***paths);
void fileops_free_paths(char **paths, int n);
/* The paths of a uri-list, else of text/plain lines that are absolute
 * paths of existing files. mime names the type of text. */
int fileops_parse_drop(const char *mime, const char *text, size_t len, char ***paths);

/* 1 when path is dir or lies below it. */
int fileops_inside(const char *path, const char *dir);
/* 1 when src and dir are on one file system, so that a move is a rename. */
int fileops_same_volume(const char *src, const char *dir);
/* The preferred action of a drag of the files of a text/uri-list into
 * the folder dest: move (GUI_DND_MOVE) when every file is on the file
 * system of dest, as in GNOME Files, copy (GUI_DND_COPY) otherwise, and
 * 0 to refuse a folder dropped into itself or files dropped into the
 * folder they are in. uris may be NULL while the list is not known yet,
 * which gives copy. */
int fileops_drop_action(const char *uris, size_t len, const char *dest);
/* Copy (GUI_DND_COPY) or move (GUI_DND_MOVE) the files into the folder
 * dir. A file copied into its own folder is named "Copy of <name>", and
 * a move into its own folder does nothing. An existing name is never
 * replaced (EEXIST), and a folder is never copied or moved into itself
 * (EINVAL). Returns the number of files copied or moved, or the first
 * error, with *failed pointing at the path that failed. */
int fileops_drop(char *const *paths, int n, const char *dir, int action, const char **failed);
