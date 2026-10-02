#pragma once
/* The host build of the toolkit tests: the configuration file functions
 * of the minios libc, implemented in host_compat.c. */
#include <stddef.h>
const char *conf_read_path(char *buf, size_t size);
const char *conf_write_path(char *buf, size_t size);
int conf_export_locale(void);
