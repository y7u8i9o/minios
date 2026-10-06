#pragma once
/* Base64 of RFC 4648, section 4, for PEM files (minios/x509.h). */
#include <stddef.h>
#include <stdint.h>

/* Decodes len characters of text into out and returns the number of
 * bytes. White space between the characters is skipped. The function
 * returns -1 for another character outside the alphabet, for padding
 * before the end, or for an incomplete last group. out needs room for
 * len / 4 * 3 bytes. */
long base64_decode(const char *text, size_t len, uint8_t *out);
