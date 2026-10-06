#pragma once
/* Intel High Definition Audio controllers (drivers/hda.c, docs/design/hda.md). */
#include <kernel.h>

/* Probe the first HD Audio controller, set up an output path on its first
 * codec that has one, and register the playback stream as the next free
 * /dev/pcmN. Called from kinit after virtio_snd_init, because the probe
 * sleeps while it waits for codec responses. */
void hda_init(void);
bool hda_present(void);
