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
