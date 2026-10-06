#pragma once
/* Intel High Definition Audio controllers (drivers/hda.c, docs/design/hda.md). */
#include <kernel.h>

/* Probe the first HD Audio controller, set up an output path on the first
 * codec that provides one, and register the playback stream as the next
 * free /dev/pcmN. kinit calls this after virtio_snd_init; the probe needs a
 * thread context because it sleeps while waiting for codec responses. */
void hda_init(void);
bool hda_present(void);
