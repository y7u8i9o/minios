# madvise (M38)

`madvise(addr, len, advice)` (`kernel/mm/madvise.c`) takes the address
space lock, checks that every page of the range belongs to a region that
is not a device mapping (`ENOMEM` for holes, `EINVAL` for devices) and then
applies the advice. Advice that changes region flags first splits the
regions at the range boundaries with `vma_split_locked`, the same helper
`munmap` and `mprotect` use, so flags never apply outside the range.

| Advice | Effect |
|---|---|
| `MADV_NORMAL`, `MADV_RANDOM`, `MADV_SEQUENTIAL` | recorded in `VM_RANDOM` and `VM_SEQUENTIAL`; nothing reads them yet |
| `MADV_WILLNEED` | every not present page is resolved as a read fault would resolve it (`vma_resolve_fault`): zero pages, swapped pages and file pages are brought in; `PROT_NONE` pages and pages beyond the end of a file are skipped |
| `MADV_DONTNEED` | `vma_unmap_range_locked` drops the frames and swap slots of the range; the next touch yields zero pages or the file's contents. Shared file pages remain in the mapping's cache and their dirty bits are recorded, so nothing written through a shared mapping is lost |
| `MADV_FREE` | private anonymous regions only (`EINVAL` otherwise); present entries lose their dirty bit and gain `PTE_LAZYFREE`, swapped entries are dropped at once |
| `MADV_DONTFORK`, `MADV_DOFORK` | set and clear `VM_DONTFORK`; `vmspace_fork` copies neither the region nor its page table entries, so the child faults on the range |
| `MADV_HUGEPAGE`, `MADV_NOHUGEPAGE` | set and clear `VM_HUGE` on private anonymous regions (M39 acts on it); `EINVAL` on file and shared regions |

## Lazy freeing

`PTE_LAZYFREE` (bit 52) marks a page the process no longer needs unless it
writes to it again. kswapd's victim scan (`find_victim` in `mm/swap.c`)
looks at the bit before anything else: a tagged page whose dirty bit is
set was written after the advice, so the tag is removed and the page is
treated normally; a tagged page that is still clean is unmapped and its
frame released instead of being written to swap, and `LazyFreed:` in
`/dev/meminfo` counts it. The next access to such a page gets a zero
page. Since kswapd runs only when free memory falls below its low
watermark, an idle system retains lazily freed pages until they are needed.

`/dev/meminfo` also gained `FileMapped:`, the number of frames referenced by
file mappings.

## Test

`tests/cases/madvise` runs `/bin/madvisetest` with a 96 MiB swap device:
every advice and its error cases on anonymous regions, `DONTNEED` on
shared and private file pages (the shared write still reaches the file),
`DONTFORK` making the range fault in a child, `MADV_FREE` followed by a
rewrite of half the pages and memory pressure that must discard only the
untouched half, `DONTNEED` and `FREE` over swapped pages releasing their
slots, and the leak checks of the `run` harness.
