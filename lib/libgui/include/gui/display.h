#pragma once
/* Display modes as X12, the desktop and the settings programs exchange
 * them. The setting display_mode of the settings interface packs a mode
 * into one integer: the scale in bits 28 to 30, the width in bits 14 to
 * 27 and the height in bits 0 to 13. Width and height are device pixels.
 * The text form is "WxH" or "WxH@S". */
#include <stddef.h>

#define DISPLAY_MODE_PACK(w, h, s) (((s) << 28) | ((w) << 14) | (h))
#define DISPLAY_MODE_W(m) (((m) >> 14) & 0x3fff)
#define DISPLAY_MODE_H(m) ((m) & 0x3fff)
#define DISPLAY_MODE_S(m) (((m) >> 28) & 7)

/* The packed mode of the text, or 0 for a text that is not a mode of at
 * least 640x480 and at most 8192x8192 with a scale from 1 to 4. A text
 * without a scale has scale 1. */
int display_mode_parse(const char *text);
/* "WxH@S" of a packed mode. */
void display_mode_format(int mode, char *buf, size_t size);
/* The resolutions that every virtio-gpu scanout accepts, as "WxH" texts
 * in increasing size. The 16 MiB buffer of the driver contains up to
 * 2560x1600. Returns the number of entries. */
int display_resolutions(const char *const **list);
