# Huge pages (M39)

## Where they come from

An anonymous private region flagged `VM_HUGE` is backed by 2 MiB frames
wherever possible. The flag comes from `mmap` with `MAP_HUGETLB` (length a
multiple of 2 MiB, private, anonymous; the region is placed 2 MiB aligned)
or from `madvise(MADV_HUGEPAGE)` on an existing region. Nothing is
allocated in advance: a not present fault inside such a region asks
`huge_fault_locked` (`kernel/mm/huge.c`) for a huge page when the 2 MiB
block around the address lies entirely inside the region and no page table
covers it, or the table left behind by earlier small pages is empty (it is
freed). The frame is an order 9 block from the buddy allocator, zeroed with
the space lock dropped, and installed as a page directory entry with
`PTE_PS`. When no order 9 block is free the fault falls back to a small
page and `HugeFallbacks:` in `/dev/meminfo` counts it.

## Invariants

A huge frame is refcounted through its head `struct page`: one reference
per page directory entry that maps it. Two rules make this simple:

1. A huge entry never straddles a region boundary. `vma_split_locked`
   splits a huge page that contains the split address before it splits the
   region, and every range operation (`munmap`, `mprotect`, `madvise`,
   `brk` shrinking) calls `huge_split_at` for its two boundaries before it
   changes anything, so a failure to split leaves the space untouched.
2. A block is mapped either as one 2 MiB page or as 512 small pages, never
   both. Splitting therefore requires exclusive ownership: an exclusively
   owned block is turned into 512 single page allocations
   (`pmm_split_block` sets their order to 0 so each frame is later freed
   on its own) and a page table whose entries point into it; a block shared
   with another space after fork is copied into 512 small frames instead
   and the huge reference dropped.

With these rules the walkers only ever meet whole huge pages:
`vma_unmap_range_locked` releases the entry with `huge_unmap_locked`,
`reprotect_range_locked` rewrites its bits, `share_level` in fork marks
it copy on write and shares it (`huge_share_locked`), kswapd's scans skip
`PTE_PS` entries, and `vmm_translate` already understood level 2 entries.
`PROT_NONE` and `MADV_FREE` need small entries (the software bits are in
not present entries and kswapd reclaims small pages), so `mprotect` to
`PROT_NONE` and `MADV_FREE` split every huge page in their range first.

## Copy on write

A write to a shared huge entry (`PTE_COW`, refcount above one) copies the
whole 2 MiB into a fresh block; when no block is available the entry is
split into small copies instead. A write to an exclusively owned entry
just regains write access, like the small page path.

## Accounting

`/dev/meminfo` reports `HugePages:` (huge entries present, counted per
space, so a page shared after fork counts twice), `HugeSplits:` and
`HugeFallbacks:`.

## Test

`tests/cases/hugepages` runs `/bin/hugetest`: `MAP_HUGETLB` argument
checks and alignment, the counts across a fork whose child writes into one
huge page, splits by a partial `munmap` and by `mprotect` with the
surrounding data intact, `MADV_HUGEPAGE` mapping exactly the aligned
interior blocks of a region, `MADV_DONTNEED` releasing a whole huge page
and the refault, the split forced by `MADV_FREE`, and the fallback after
physical memory has been fragmented by touching nearly every frame and
releasing every other one.
