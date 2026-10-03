# ELF loader, initrd, fork and exec (M9)

## Initrd

`limine.conf` loads `/boot/initrd.tar` as a module. `boot_init` records
its address (already in the direct map, in an "executable and modules"
region that is never reclaimed) and size. `fs/initrd.c` parses the ustar
archive once into a static table of up to 256 entries with normalized
names (`bin/init`), a type and a pointer into the archive. `initrd_lookup`
is the only file lookup until the VFS (M11). `make initrd` packs
`build/initrd_root`, which `user/` populates with every program.

## Regions and demand paging

`struct vmspace` now carries a sorted list of `struct vma` regions and the
heap break. Pages inside a region are not allocated up front: the first
touch faults, `vmm_handle_fault` finds the region under `vm->lock`, checks
the access against the region flags and maps a zeroed frame. Kernel code
that writes into user memory before the process runs (ELF loading, the
initial stack) calls `vma_populate` first. Kernel accesses from system
calls fault the same way because the process address space is active;
`user_range_ok` verifies beforehand that the range lies inside regions. Since
M43 that validation walks the RCU-published VMA forward links without taking
`vmspace.lock`; removed VMAs and their file references are reclaimed after a
grace period.

`struct page` gained a 32 bit `refcount` (flags shrank to 16 bits). A frame
mapped into user space contains one reference per mapping; `page_put` frees
the frame when the last reference goes.

## fork with copy on write

`vmspace_fork` copies the region list and walks the parent's lower half
page tables. Every present writable page loses `PTE_W` and gains the
software bit `PTE_COW` in both parent and child; read only pages are
simply shared. The frame's refcount is incremented and the parent's TLB
is flushed. A write fault on a `PTE_COW` page with refcount 1 restores
write access in place, otherwise it copies the frame into a fresh one and
drops the old reference. `CR0.WP` (set in M4) makes kernel writes through
user mappings take the same path.

`proc_fork` copies the parent's trap frame, saves the parent's FPU state
into the child's area and copies the FS base, and only then queues the
child with `sched_add`. With several CPUs the child can start on another
CPU as soon as it is queued, so everything it reads in `thread_start`
must be complete before that call.

## ELF loading and the initial stack

`sched/elf.c` accepts `ET_EXEC` x86_64 files. Each `PT_LOAD`
segment becomes a region rounded to pages, populated and filled from the
file, then protected to the segment's permissions. A one page heap region
follows the highest segment; `sbrk` grows it with `vma_brk`. The stack
region is 1 MiB below `USER_STACK_TOP`, demand paged; `user_stack_setup`
copies the strings, builds `argc`, `argv`, `NULL`, `envp`, `NULL`, the
auxiliary vector, and leaves the stack pointer 16 byte aligned, as the
x86-64 and AArch64 ABIs require (before A6 an adjustment left it 8 bytes
off, which only the realignment in the x86 start code hid). When no
`PT_LOAD` segment covers the program headers, which the bare-metal aarch64
linker script does for a static program, the headers are copied to the top
of the stack and `AT_PHDR` points to the copy. `crt0.S` reads that layout. A program with a `PT_INTERP` header
is dynamically linked: the loader it names is mapped at
`USER_INTERP_BASE` and entered first (`dynlink.md`).

## Processes

`struct proc` has a parent, a children list, an `exiting` flag and a
wait4 style `exit_status`. Parent, children and state are protected by the
global `proc_tree_lock`. `proc_begin_exit` records the status, sets
`exiting` and interrupts every other thread of the process blocked in the
kernel (`waitq_interrupt`); each thread notices at its next kernel exit
(`proc_exit_check` in the syscall and interrupt return paths) and exits.
The last thread out calls `proc_exit_notify`, which marks the zombie,
reparents children to init and wakes the parent's `child_waitq`.

`wait4` scans the caller's children for a zombie, reaps it (`proc_reap`
waits for every thread to have switched away, frees them, tears down the
address space) and returns the pid with the status. `kill` is terminate
only: it calls `proc_begin_exit` with a signalled status. Faults in user
mode use the same path with `SIGSEGV`. `execve` copies path, argv and envp
into kernel memory, builds a new address space, swaps it in, destroys the
old one and rewrites the syscall frame to enter the new program; it is
refused with `-EBUSY` while other threads exist.

## User threads

`thread_create(entry, arg, stack_top)` starts a thread in the calling
process with `rdi = arg`; `thread_exit(code)` ends it; `thread_join(tid)`
waits for the `finished` flag, returns the code and frees the thread.
Exited threads wait on `proc->zombies` until joined or until the process
is reaped. M35 adds thread local storage (`set_tls`, the FS base retained per
thread), `gettid` and futexes; `docs/design/threads.md` describes them and
the POSIX interface in libc.

## System calls added

`fork` (5), `execve` (6), `wait4` (7), `kill` (8), `getppid` (9),
`thread_create` (10), `thread_exit` (11), `thread_join` (12), `read` (13,
descriptor 0 reads completed keyboard lines, blocking and killable),
`sbrk` (14), `chdir` (15) and `getcwd` (16). `chdir` resolves `.` and
`..` against the process cwd and checks the initrd.

## Boot

`kinit` starts `/bin/init` with `PATH=/bin`, registers it as the
reparenting target and reaps it, which is a panic. The embedded flat binary
of M8 is gone.

## Test

`tests/cases/fork` boots with `test=run prog=/bin/forktest`. The program
checks its arguments and environment, forks and verifies copy on write in
both directions across a 256 KiB array, the heap and the stack, execs
`/bin/hello` with arguments, checks `execve` failure, kills a spinning
child and a child blocked in `read`, checks the `SIGSEGV` status of a
crashing child, `ECHILD`, two threads incrementing a counter, a 300 KiB
stack frame, a 3 MiB `malloc` and `chdir`/`getcwd`. The kernel then
verifies that the exit status is 0 and that every physical page was
returned.
