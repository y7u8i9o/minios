#pragma once
#include <kernel.h>

/* Kernel command line handling. The command line is a space separated list of
 * words, each either `key` or `key=value`. */
void cmdline_init(const char *cmdline);
const char *cmdline_get(void);
/* Copies the value of key into buf. Returns true if the key was present. A key
 * without a value yields an empty string. */
bool cmdline_lookup(const char *key, char *buf, size_t size);
