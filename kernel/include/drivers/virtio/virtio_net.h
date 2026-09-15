#pragma once
#include <kernel.h>
void virtio_net_init(void);
/* Called by netd once per bounded pass, never under worker.lock. */
void virtio_net_service(void);
/* Worker-only stop; test/control seam, ends DMA before freeing storage. */
int virtio_net_stop(void);
bool virtio_net_header_valid(const uint8_t *data, uint32_t len, uint32_t capacity);

#if CONFIG_TESTS
/* Inspect cleanup after a boot-time partial initialization fault. */
bool virtio_net_test_clean(void);
#endif
