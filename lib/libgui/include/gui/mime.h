#pragma once
/* File types by name: /etc/mime.types maps extensions to types and
 * /etc/mime.apps maps types to the command that opens them.  A command is
 * a program followed by arguments separated by spaces.  The path of the
 * file follows the arguments. */
#include <sys/types.h>

#define MIME_MAX 64
#define MIME_COMMAND 128
#define MIME_LAUNCHER "application/x-launcher"   /* .app files: "exec=/bin/prog" */
#define MIME_DIRECTORY "inode/directory"

/* Load the tables (done on first use with the default paths).  A NULL
 * path reloads the table from the path of an earlier call, else from the
 * default path. */
int mime_load(const char *types_path, const char *apps_path);
/* Type of a path from its extension (a directory when is_dir), or
 * "application/octet-stream". */
const char *mime_type(const char *path, int is_dir);
/* Command for a type: the entry for the type, then the entry for the
 * major type followed by "/" and an asterisk, then the entry "*".  The
 * result is NULL when nothing matches. */
const char *mime_handler(const char *type);
/* mime_handlers stores up to max commands registered for a type in list,
 * in the order of mime_handler, each command once.  The list includes
 * the package handlers that the system or user table overrides.  The
 * result is the number of commands. */
int mime_handlers(const char *type, const char **list, int max);
void mime_set_handler(const char *type, const char *program);
/* Save the handler table to apps_path (NULL: the loaded path). */
int mime_save(const char *apps_path);
/* Handler table iteration for settings editors. */
int mime_handler_count(void);
const char *mime_handler_type(int index);
const char *mime_handler_program(int index);
/* Icon name (in /usr/share/icons) for a type. */
const char *mime_icon(const char *type);
/* Start the command for path through mime_run.  Returns 1 or -errno.  A
 * launcher file runs its exec= line instead. */
pid_t mime_open(const char *path);
/* mime_run starts command through mime_spawn.  path follows the
 * arguments of the command when path is not NULL.  Returns 0 or -errno. */
int mime_run(const char *command, const char *path);
/* Start argv[0] with argv as a child of init.  The function forks twice
 * and reaps the intermediate child at once.  init then reaps the program
 * when it exits, and a window without a wait loop has no zombie child.
 * Returns 0, or -errno when the fork or the exec of argv[0] fails. */
int mime_spawn(char *const argv[]);
