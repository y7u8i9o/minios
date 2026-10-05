#pragma once
#include <kernel.h>

/* virtio-balloon (V4 of docs/plan/release-0.6.0.md, docs/design/balloon.md).
 * The host sets a target size in its configuration space. The driver
 * inflates the balloon to the target by giving free pages to the host and
 * deflates it by returning them to the allocator. It answers the requests
 * of the host for memory statistics. With VIRTIO_BALLOON_F_DEFLATE_ON_OOM
 * the allocator takes pages from the balloon before an allocation fails or
 * kswapd evicts pages. /dev/balloon reports the size and the target. */
void virtio_balloon_init(void);

struct balloon_info {
    bool present;
    bool deflate_on_oom;            /* the feature was negotiated */
    uint64_t size;                  /* pages in the balloon */
    uint64_t target;                /* pages that the host asks for */
    uint64_t released_on_pressure;  /* pages returned to the allocator under pressure since boot */
    uint64_t stats_updates;         /* statistics sent to the host */
};
void virtio_balloon_get_info(struct balloon_info *out);
