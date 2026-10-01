# File backed mappings and mprotect (M37)

## Regions

`struct vma` (`kernel/include/mm/vma.h`) gained three fields for file
regions: the open `file`, the file `offset` of the region start and a
reference on the file's `struct mapping`. `VM_FILE` marks such a region,
`VM_SHARED` distinguishes `MAP_SHARED` from `MAP_PRIVATE`. `sys_mmap`
(`kernel/syscall/sys_mm.c`) routes regular files to `mmap_regular` when the
file has no `mmap` operation of its own; devices and shared memory objects
keep their driver hook. A shared writable mapping needs a descriptor open
for reading and writing without `O_APPEND`, otherwise `EACCES`. Anonymous
`MAP_SHARED` regions are backed by an unnamed shared memory object (the
memfd machinery), so fork shares their frames. `MAP_FIXED` unmaps whatever
mmap regions the range holds and refuses ranges that touch the program
image, heap or stack.

Regions are split by `vma_split_locked` when `munmap` or `mprotect` cuts
through them; the tail takes its own file and mapping references and an
adjusted offset. Regions are freed by `vma_release` after the space lock is
dropped, because releasing the file may sleep.

## The mapping

`struct mapping` (`kernel/include/mm/filemap.h`, `kernel/mm/filemap.c`) is
the cache of the pages of one file that are mapped somewhere. `inode->mapping`
points to it while at least one region references it; the last
`filemap_put` writes nothing, warns about dirty pages that were not written
back, drops the frames and clears the pointer, so an unmapped file has no
cache. `pages` is indexed by page offset, `dirty` is a bitmap, both grow on
demand. Every cached frame carries one reference from the mapping plus one
per page table entry, which keeps kswapd away from it (it evicts only
frames with a single reference) and lets `vma_unmap_range_locked` drop
frames uniformly with `page_put`.

## Faults

`vmm_handle_fault` hands a not present fault inside a `VM_FILE` region to
`filemap_fault`, which releases the space lock, takes references on the
mapping and the file, and asks `mapping_get_page` for the page. A missing
page is read through the file's `read` operation into a frame from
`swap_alloc_user_frame` and zero filled past the end of the file; a page
offset at or beyond the end of the file (rounded up to a page) returns
nothing and the process gets `SIGSEGV`. A shared region maps the cached
frame with the region's protection. A private region maps it read only
with `PTE_COW` on a read fault and copies it into a private frame on a
write fault; the existing copy on write path then handles later writes
because the cached frame always has at least two references. After the
I/O the handler retakes the lock, checks that the region is still the
same one and the entry still empty, and installs the entry.

## Dirty pages and writeback

Writes through a shared mapping set the hardware dirty bit. The bit is
gathered into the mapping's bitmap at three points: `vma_unmap_range_locked`
while it drops the entries of a shared file region, `vma_msync` over the
requested range (which also clears the bit and flushes the TLB so the next
write dirties the page again) and `reprotect_range_locked` when a region
loses read access. The unmap and msync paths queue a `sync_job` per range
while the space lock is held and run the jobs afterwards:
`filemap_writeback` writes each dirty page through the region's file
inside `vfs_op_begin`/`vfs_op_end`, clamped to the file size so the zero
tail of the last page never extends the file. Process exit and exec go
through `vma_remove_all`, so a mapping left alive at exit is written back
too.

## Coherence with read and write

`file_read` overlays cached pages onto the data the filesystem returned
(`filemap_read_overlay`), so a `read` sees writes made through a shared
mapping before they are written back. `file_write` copies the written
bytes into cached pages (`filemap_write_through`), so mappings see ordinary
writes. `O_TRUNC` drops cached pages beyond the new size and zeroes the
tail of the last kept page (`filemap_truncate`); mappers keep their old
entries, which is the documented difference from a `SIGBUS` on Linux.

## mprotect and PROT_NONE

`vma_mprotect` requires the whole range to be mapped (`ENOMEM`), refuses
device regions (`EACCES`) and a write upgrade of a shared file region whose
descriptor cannot write, splits the regions at the boundaries and rewrites
their present entries. An entry keeps `PTE_COW` and never gains `PTE_W`
while the bit is set. Because a later `mprotect` may add write access,
`vmspace_fork` now marks every frame of a private region copy on write,
whether or not it was writable.

`PROT_NONE` cannot be expressed by x86 protection bits, so the frame stays
attached to a not present entry tagged with the software bit `PTE_PROTNONE`
(bit 53). The fault handler refuses access to regions without `VM_READ`,
`vma_range_ok` refuses them for kernel accesses, unmapping and fork treat
them like present entries (`pte_mapped`) and `vma_make_pte` produces the
form for any region protection, so swap in and populate install the right
kind of entry.
A later `mprotect` makes the entries present again.

## Test

`tests/cases/mmap_file` runs `/bin/mmapfiletest` under the `run` harness,
which also checks that every page is returned afterwards.
