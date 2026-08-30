#pragma once
/* File types by name: /etc/mime.types maps extensions to types and
 * /etc/mime.apps maps types to the program that opens them. */
#include <sys/types.h>

#define MIME_MAX 64
#define MIME_LAUNCHER "application/x-launcher"   /* .app files: "exec=/bin/prog" */
#define MIME_DIRECTORY "inode/directory"

/* Load the tables (done on first use with the default paths). */
int mime_load(const char *types_path, const char *apps_path);
/* Type of a path from its extension (a directory when is_dir), or
 * "application/octet-stream". */
const char *mime_type(const char *path, int is_dir);
/* Program for a type: the entry for the type, then for "type/*", then
 * for "*"; NULL when nothing matches. */
const char *mime_handler(const char *type);
void mime_set_handler(const char *type, const char *program);
/* Save the handler table to apps_path (NULL: the loaded path). */
int mime_save(const char *apps_path);
/* Handler table iteration for settings editors. */
int mime_handler_count(void);
const char *mime_handler_type(int index);
const char *mime_handler_program(int index);
/* Icon name (in /usr/share/icons) for a type. */
const char *mime_icon(const char *type);
/* Start the program for path in a child process; returns its pid or
 * -errno. A launcher file runs its exec= line instead. */
pid_t mime_open(const char *path);
