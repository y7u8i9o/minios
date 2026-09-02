#pragma once

/* Probe modern virtio-sound PCI functions and register playback PCM
 * devices as /dev/pcm0, /dev/pcm1, ... */
void virtio_snd_init(void);
