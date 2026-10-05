#pragma once
/* The read only ISO 9660 file system with Rock Ridge
 * (fs/iso9660/iso9660.c, docs/design/iso9660.md). */
#include <kernel.h>

struct blockdev;

/* Register the file system type "iso9660". */
void iso9660_init(void);
/* True when dev contains an ISO 9660 file system whose volume identifier
 * is label, compared without its trailing blanks. Used by root=LABEL=. */
bool iso9660_has_label(struct blockdev *dev, const char *label);
