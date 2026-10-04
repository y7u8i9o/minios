#pragma once
#include <kernel.h>

/* GUIDs in the mixed byte order of GPT and SMBIOS 2.6 and later: the
 * first three fields little endian, the last two as stored. */
#define GUID_STR 37                 /* 36 characters and the NUL */

/* Format a GUID as a lowercase string. */
void guid_format(const uint8_t guid[16], char out[GUID_STR]);
/* Parse the standard string form of a GUID. Returns 0 or -EINVAL. */
int guid_parse(const char *s, uint8_t out[16]);
/* True if every byte of the GUID is 0. */
bool guid_is_zero(const uint8_t guid[16]);
