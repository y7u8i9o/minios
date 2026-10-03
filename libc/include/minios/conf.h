#pragma once
#include <stddef.h>

/* The configuration of the desktop session. /etc/desktop.conf ships the
 * defaults; the user's file $HOME/.config/desktop.conf, written by the
 * settings program, overrides it. Programs read through conf_read_path,
 * which names the user's file when it exists and the default otherwise,
 * and write through conf_write_path, which creates $HOME/.config. Both
 * copy the path into buf and return it. */
const char *conf_read_path(char *buf, size_t size);
const char *conf_write_path(char *buf, size_t size);

/* The same rule for any file of the user's configuration directory:
 * conf_user_file names $HOME/.config/NAME when it exists and default_path
 * otherwise, conf_user_write_file names $HOME/.config/NAME and creates
 * $HOME/.config. The launcher menu and the MIME handlers follow it
 * (docs/design/users.md). */
const char *conf_user_file(const char *name, const char *default_path, char *buf, size_t size);
const char *conf_user_write_file(const char *name, char *buf, size_t size);

/* The home directory of the caller: HOME when it is an absolute path,
 * else the home of the real uid in /etc/passwd, else "/"
 * (docs/design/users.md). */
const char *conf_home(void);

/* conf_export_locale sets LANG from the lang setting and LC_NUMERIC,
 * LC_TIME and LC_MONETARY from the formats setting. Without formats the
 * three categories are removed and follow LANG. Without lang the
 * environment stays as it is. It returns 1 when a variable changed, else
 * 0 (docs/design/desktop.md). */
int conf_export_locale(void);
