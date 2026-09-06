#pragma once

/* Device numbers: the major number in the high 32 bits, the minor in the
 * low 32 bits, the layout of rdev in minios. */
#define major(dev) ((unsigned)(((unsigned long long)(dev) >> 32) & 0xffffffffu))
#define minor(dev) ((unsigned)((dev) & 0xffffffffu))
#define makedev(maj, min) (((unsigned long long)(maj) << 32) | ((unsigned long long)(min) & 0xffffffffu))
