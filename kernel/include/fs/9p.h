#pragma once
/* The filesystem 9p: folders of the host through virtio-9p (V5 of
 * docs/plan/release-0.6.0.md, docs/design/9p.md). The source of a mount is
 * the mount tag of a virtio-9p device. */
void p9fs_init(void);
