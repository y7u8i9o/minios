# Memory management extensions: mmap and swap

## mmap and munmap

`mm/mmap.c` adds anonymous private mappings to the region model of M9.
`vma_mmap` picks the highest free gap below `USER_MMAP_TOP`
(`0x7f0000000000`, below the stack) or honours a page aligned hint when
that range is free, and adds a region tagged `VM_MMAP`. Pages are
demand allocated by the fault handler like every other region.
`vma_munmap` unmaps the frames and swap slots of the overlap and shrinks,
splits or removes the regions involved; regions that were not created by
`mmap` (text, data, heap, stack) are refused with `EINVAL` so the heap
bookkeeping of `brk` stays valid. Only `MAP_ANONYMOUS | MAP_PRIVATE` is
supported: file mappings return `ENODEV`, `MAP_FIXED` returns `EINVAL`.
`brk` and `sbrk` are unchanged.

Since M46, a multi-page unmap clears the complete range while holding the
space lock and then issues one range TLB shootdown. Resident accounting is
updated through the address space's per-CPU counter as present entries are
removed.

## Swap device and slots

`mm/swap.c` uses the swap partition of the boot disk (P4,
`block.md`), or else the second virtio-blk device, `vdb`, when it has no
partition table, as swap space without any on disk format: slot `n` occupies the eight sectors starting
at `8n`, slot 0 is never used so a zero entry means "no slot". A bitmap
under `swap_lock` tracks free slots; `swap_alloc_run` hands out up to 32
consecutive slots so a batch of evicted pages is written with one
request. `make disk` creates `build/swap.img` (64 MiB by default,
`SWAP_MB`) and `make run` attaches it. The test runner attaches a zero
filled image of the size named in a case's `swap` file. `swap=off` on the
command line attaches no device, which the installer environment uses
(`installer.md`).

## Page table representation

A swapped out page has a not present entry with the software bit
`PTE_SWAPPED` (bit 10) and the slot number in the address bits. Every
path that walks user page tables understands the encoding: unmapping a
region or destroying a space frees the slot, `fork` first brings every
swapped page back (see below), and the fault handler swaps in.

## Eviction

`kswapd`, a kernel thread started once the scheduler runs, polls the free
page count every 10 ms. Below `WATERMARK_HIGH` (768 pages) it runs
`evict_batch` until the count recovers. The clock hand is a pair (address
space, address) kept across calls. Address spaces are registered in
`vmspaces` under `vmspaces_lock`; the hand walks them round robin and
within one space scans the lower half page tables with a budget of 2048
entries per lock hold. A candidate is a present user page whose frame
has reference count 1 (frames shared copy on write are skipped) and
whose accessed bit is clear; accessed bits found set are cleared, with a
TLB flush, so the page becomes a candidate on the next pass. Spaces
marked `pinned` are skipped.

For each victim the entry is rewritten to the swapped form and the TLB
flushed while holding the space lock; the frame stays allocated. After
the locks are dropped, the batch is copied into one contiguous buffer,
written with one request under `swap_io_lock`, and the frames are
released. With no large buffer available the pages are written one at a
time.

User frames are allocated through `swap_alloc_user_frame`, which refuses
to go below `WATERMARK_RESERVE` (256 pages) while swap is enabled and
waits on `swap_waitq` for kswapd to make progress. The reserve keeps the
kernel allocations of the swap path itself (request structures, bounce
buffers) from failing. The fault handler drops the space lock around the
allocation and re-checks the entry afterwards, so a page that appeared
meanwhile is not mapped twice.

## Swap in

A fault on a swapped entry calls `swap_in_page`. It reads the slot,
extends the request to up to 16 following pages whose slots are
consecutive (pages evicted together are usually used together), allocates
the frames, reads the run with one request under `swap_io_lock`, then
re-locks the space and installs every entry that still refers to the
expected slot, freeing the slot and counting the swap in. Frames not
needed any more are returned. `swap_io_lock` serializes swap I/O so a
read never observes a slot whose write is still in flight.

`fork` calls `swap_in_all` and sets `pinned` on the parent until the copy
is done, because the copy on write sharing walks present entries only.

## Blocking in the fault handler

A fault may now block: on the frame allocation, on the swap mutex and
on the device. Faults on user memory raised by kernel code therefore
must not happen while a spinlock is held. The pipe and keyboard paths
copy through bounce buffers with their locks dropped and `getcwd` copies
the path outside `proc.lock`; every other user access already happened
without a spinlock.

## Inspection

`/dev/meminfo` reports `MemTotal`, `MemFree`, `SwapTotal`, `SwapFree`,
`SwappedOut` and `SwappedIn`. The `swap` test reads it.

## Test

The `swap` case boots with 128 MiB of RAM and a 96 MiB swap image and
runs `/bin/swaptest`, which checks `mmap` and `munmap` semantics (zero
fill, alignment, partial unmap, refusal of non mmap regions, hints,
copy on write across `fork`), then maps 1.25 times the physical memory,
fills every page with an offset dependent pattern, verifies it, repeats
with a second pattern and checks the swap counters. The kernel side
verifies that every frame and slot is returned after the process exits,
after waiting for an in flight eviction batch with `swap_drain`.

Since M38 the victim scan also discards clean pages tagged by `MADV_FREE`
instead of writing them out (see `madvise.md`).
