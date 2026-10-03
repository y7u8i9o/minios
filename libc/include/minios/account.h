#pragma once
/* Editing the account databases (docs/design/users.md). login, su,
 * passwd, useradd, userdel and the Users page of the settings program use
 * these helpers. */
#include <stdbool.h>
#include <stddef.h>

#define ACCOUNT_PASSWD "/etc/passwd"
#define ACCOUNT_GROUP  "/etc/group"
#define ACCOUNT_SHADOW "/etc/shadow"
#define ACCOUNT_SKEL   "/etc/skel"

/* The first uid and gid given to accounts made by useradd. */
#define ACCOUNT_FIRST_ID 1000

/* Replace the line of the file at path whose first field is name with
 * line (given without its newline), append it when there is none, or
 * remove it when line is NULL. A symbolic link at path is followed, and
 * the new contents are written to a temporary file in the directory of the
 * target and renamed over it, which keeps the mode and the owner of the
 * old file. Returns 0 or -1 with errno set. */
int account_replace(const char *path, const char *name, const char *line);

/* Write a SHA-256 crypt hash of password with a fresh random salt to out.
 * Returns 0 or -1 with errno set. */
int account_hash(const char *password, char *out, size_t size);

/* True if password matches the stored hash. An empty hash accepts only an
 * empty password, and a locked hash ("!" or "*") accepts nothing. */
bool account_check(const char *password, const char *hash);

/* True if name is a valid account or group name: a lowercase letter, then
 * up to 31 lowercase letters, digits, "_" or "-". */
bool account_name_valid(const char *name);

/* Copy the tree below from into the existing directory to, keeping the
 * permission bits less the umask and copying symbolic links as links.
 * Entries that exist already are kept. Returns 0 or -1 with errno set. */
int account_copy_tree(const char *from, const char *to);

/* Give path and, when it is a directory, everything below it to uid and
 * gid, changing symbolic links themselves. Returns 0 or -1. */
int account_chown_tree(const char *path, unsigned uid, unsigned gid);

/* Make the home directory dir of an account: mode 0700, the contents of
 * ACCOUNT_SKEL, everything owned by uid and gid. A dir that exists is left
 * as it is. Returns 0 or -1 with errno set. */
int account_make_home(const char *dir, unsigned uid, unsigned gid);

/* Read one line into buf without its newline. On a terminal the prompt is
 * written to standard error and echo is off while the line is typed,
 * otherwise the line is read from standard input without a prompt, which
 * lets a program pass a password through a pipe. Returns 0, or -1 at the
 * end of the input. */
int account_read_password(const char *prompt, char *buf, size_t size);

/* The days since the epoch, for the last change field of /etc/shadow. */
long account_today(void);
