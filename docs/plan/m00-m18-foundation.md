# Milestones M0 to M18: the kernel foundation

Each milestone ends with a visible result and a boot test in `tests/cases/`. Milestones are ordered by dependency. Items within a milestone may be reordered.

## M0. Toolchain and build skeleton (completed 2026-08-29)
- `toolchain.mk` with `x86_64-elf-gcc`, `-ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mcmodel=kernel -mno-sse -mno-mmx -O2 -g`.
- Download Limine binaries into `third_party/limine/`, `limine.conf` with a single entry.
- `linker.ld` for a higher half kernel with `.text`, `.rodata`, `.data`, `.bss`, `.ksyms` sections, 4 KiB aligned.
- `make image` produces `build/minios.iso` (initial) and later `build/disk.img`.
- `make run` starts QEMU with `-serial stdio -m 512M -M q35`.
- `make gdb` starts QEMU with `-s -S` and prints the GDB command line. `.gdbinit` loads symbols and connects.
- `make test` runs `tests/run_qemu_test.sh` against every case.
- Result: kernel entry reached, "minios booting" printed on serial.

## M1. Early kernel and console (completed 2026-08-29)
- Serial driver (COM1, polled output).
- `kprintf` with `%d %u %x %p %s %c %lu %lx %ld` and width padding.
- Framebuffer console using the Limine framebuffer request, 8x16 bitmap font, scrolling, cursor.
- Console layer writing to both serial and framebuffer.
- GDT with kernel and user code and data segments and a TSS.
- IDT with all 32 exception handlers. Exception handler prints registers and panics.
- `panic()` implemented in `debug/panic.c`, initially printing message and halting.
- `arch/x86_64/power.c` with `power_off()` and `power_reboot()`. Power off writes `0x2000` to port `0x604` (the QEMU q35 ACPI PM1a control port), with `isa-debug-exit` on port `0xf4` as the fallback used by tests to return an exit code. Reboot pulses the 8042 keyboard controller reset line (`0xFE` to port `0x64`) and falls back to a triple fault through an empty IDT. Both are unconditional here, the orderly sequence is added in M15.
- Panic in headless test runs calls `power_off()` with a failure exit code instead of halting, controlled by the `CONFIG_PANIC_EXIT` option.
- Result: exceptions produce a readable register dump.

## M2. Debugging infrastructure (completed 2026-08-29)
- `.ksyms` section populated by `tools/gensyms` in a second link pass (link, extract symbols with `nm`, relink with the symbol blob).
- Backtrace walking frame pointers (`-fno-omit-frame-pointer`), printing symbolized addresses on panic and on unhandled exceptions.
- `panic()` extended with register dump and backtrace to serial and framebuffer. Current thread and process are added to the panic output in the scheduler milestone.
- Kernel log levels (`LOG_DEBUG` to `LOG_ERROR`) with a compile time and runtime threshold, `klog(level, ...)` macro with subsystem prefix.
- `kassert` macro that panics with file, line and expression.
- `.gdbinit` with symbol loading, connection to the gdbstub, and helper commands extended in later milestones (`ps`, `bt-thread`, `pmm-stats`).
- Boot test that deliberately triggers a page fault in a nested call chain and checks that the serial output contains the expected function names.
- `struct cpu` (id, current thread, kernel stack, interrupt disable depth, local APIC id) allocated statically for the boot CPU and reached through the `GS` base via `cpu_current()`. No global "current thread" variable is ever introduced.
- `spinlock` implemented with `lock xchg` or `lock cmpxchg`, `spin_lock` disables interrupts and records the previous state with a nesting count in `struct cpu` (`push_cli` / `pop_cli`), `spin_unlock` restores it. Debug builds record the owning CPU and the caller address, and detect double acquisition and unlock by a non owner.
- `spinlock_irqsave` variants for data shared with interrupt handlers, plain spinlocks for data only touched in thread context.
- `docs/design/locking.md` started here and maintained in every later milestone: one line per lock, what it protects, and the global lock ordering. Every later milestone adds its locks to this file before the code is written.

## M3. Physical memory: buddy allocator (completed 2026-08-29)
- Parse the Limine memory map. Use the Limine HHDM (higher half direct map) for physical access.
- Buddy allocator with orders 0 to 10 (4 KiB to 4 MiB), one free list per order, per page `struct page` array with order and flags.
- `pmm_alloc(order)`, `pmm_free(page, order)`, `pmm_alloc_page()`, statistics dump.
- Free lists protected by a single `pmm_lock` spinlock from the first version. Per CPU page caches can be added in M17 without changing the interface.
- Boot test: allocate and free patterns, verify coalescing by checking free counts.

## M4. Virtual memory (completed 2026-08-29)
- Own page tables (PML4) replacing Limine's, kernel mapped in the higher half, HHDM preserved, framebuffer mapped write combining.
- `vmm_map`, `vmm_unmap`, `vmm_protect`, `vmm_translate`, address space creation and destruction.
- Page fault handler reporting faulting address (`CR2`), error code and instruction pointer.
- Each address space carries its own lock. Kernel page table modifications go through a single `kvm_lock`. Every unmap and protect call goes through `tlb_flush_range`, which flushes locally now and becomes the IPI shootdown entry point in M17, so no caller needs to change.
- Kernel stack guard pages.
- Result: page faults on unmapped kernel addresses are reported correctly.

## M5. Kernel heap: slab allocator (completed 2026-08-29)
- Slab caches with fixed object sizes, `kmem_cache_create`, `kmem_cache_alloc`, `kmem_cache_free`, slabs backed by buddy pages, free list within each slab.
- `kmalloc` and `kfree` built on size class caches (16 B to 8 KiB), larger requests fall through to the buddy allocator.
- Poisoning on free in debug builds, redzone check.
- One spinlock per cache. Per CPU magazines are an optional M17 addition behind the same `kmem_cache_alloc` interface.
- Boot test: allocation stress with pattern verification.

## M6. Interrupts and timing (completed 2026-08-29)
- Local APIC enabling, PIC disabling, spurious interrupt vector.
- I/O APIC redirection for the PS/2 keyboard and PIT.
- PIT used once to calibrate the APIC timer frequency, then APIC timer configured at 1000 Hz.
- `sleep_ms` busy variant, monotonic tick counter.
- PS/2 keyboard driver with scancode set 1 to keycode translation, shift and control handling, line discipline buffer.
- Result: keys typed in QEMU are echoed to the console.

## M7. Kernel threads and scheduler (completed 2026-08-29)
- `struct thread` (kernel stack, saved context, state, priority level, time slice remaining, owning process) and `struct proc` (address space, thread list, pid, parent, file table, cwd, exit status).
- Context switch in assembly (`context.S`), saving callee saved registers and `rsp`.
- MLFQ: 8 queues, time slice doubling per level (10 ms at the top), demotion on slice exhaustion, promotion on block, periodic priority boost every 1 s to prevent starvation.
- `mutex`, `semaphore`, `condvar` built on a wait queue, each internally protected by a spinlock from M2. Blocking primitives are never used from interrupt context.
- The original run queue was protected by `sched_lock`. M18 introduced per CPU queues behind `sched_pick_next`; M44 replaced the remaining global scheduler lock with one lock and one remote-wake MPSC inbox per CPU. The scheduler always reads `cpu_current()->thread`, never a global current-thread pointer.
- Idle thread, `thread_create`, `thread_exit`, `yield`, `sleep_ms` blocking variant.
- Result: several kernel threads print interleaved output at different priorities.

## M8. User mode and system calls (completed 2026-08-29)
- User code segment, `syscall` MSR setup (`STAR`, `LSTAR`, `SFMASK`), `syscall.S` entry saving user state, swapping to the kernel stack via the per CPU area and `swapgs`.
- Syscall table with numbers in `kernel/include/syscall_nums.h`, shared with libc.
- Initial syscalls: `write`, `exit`, `getpid`, `yield`.
- First user program embedded in the kernel image as a flat binary, then replaced by ELF loading in M9.
- Result: a ring 3 program calls `write` and `exit`.

## M9. ELF loader, initrd, fork and exec (completed 2026-08-29)
- Initrd (tar or cpio) loaded by Limine as a module, exposed as a read only filesystem.
- ELF64 loader: program headers, `PT_LOAD` mapping, stack setup with `argc`, `argv`, `envp`.
- `fork` with copy on write: page table copy marking writable pages read only and tagged COW, per page reference count in `struct page`, fault handler copies on write.
- `execve`, `wait4`, `exit`, `kill` (terminate only), `getppid`.
- User threads: `thread_create` syscall (`clone` style with a new stack), `thread_exit`, `thread_join`.
- Result: `init` forks and executes a second program from the initrd.

## M10. libc and the first user programs (completed 2026-08-29)
- `crt0.S`, syscall wrappers, `errno`.
- `string.h` complete, `stdio.h` (`printf`, `fprintf`, `snprintf`, `puts`, `getchar`, `fgets`, buffered `FILE`), `stdlib.h` (`malloc` via `sbrk` or `mmap`, `atoi`, `strtol`, `exit`, `abort`), `unistd.h`, `fcntl.h`, `dirent.h`, `sys/wait.h`, `sys/stat.h`, `errno.h`, `assert.h`, `ctype.h`.
- `init` and a minimal `sh` (command line parsing, `PATH` lookup, `fork`/`exec`/`wait`, `cd`, `exit`).
- Result: interactive shell running programs from the initrd.

## M11. VFS (completed 2026-08-29)
- `struct inode`, `struct dentry` (or a simple path cache), `struct file`, `struct fs_ops`, `struct inode_ops`, `struct file_ops`.
- Path resolution with `.` and `..`, relative to cwd, mount point crossing.
- Mount table, `mount`, `umount`, root mount of the initrd.
- devfs with `/dev/console`, `/dev/null`, `/dev/zero`, `/dev/kbd`, later `/dev/vda`.
- File descriptor table per process, `open`, `close`, `read`, `write`, `lseek`, `dup`, `dup2`, `stat`, `fstat`, `getdents`, `mkdir`, `unlink`, `rmdir`, `chdir`, `getcwd`, `rename`.
- Pipes and `pipe`, shell redirection (`<`, `>`, `|`).
- Result: `ls /dev`, `cat file | wc` work.

## M12. PCI and virtio-blk (completed 2026-08-29)
- PCI enumeration over the configuration space, device listing on boot.
- virtio over PCI (modern interface), virtqueue implementation, virtio-blk driver with interrupt driven completion.
- Block layer: `struct blockdev`, request queue, block cache (write back, LRU, `sync` syscall).
- `/dev/vda` in devfs.
- Result: raw sector reads and writes through the block cache.

## M13. mfs filesystem (completed 2026-08-29)
- On disk layout: superblock, inode bitmap, block bitmap, inode table, data blocks. Inodes with 12 direct, one indirect and one double indirect pointer. Directories as arrays of fixed size entries. Block size 4 KiB.
- `tools/mkfs` host tool building an image from a directory tree.
- Kernel implementation: superblock reading, inode read and write, block allocation, directory lookup, create, unlink, truncate, rename, hard links.
- Root switched from initrd to `/dev/vda` (`disk.img` becomes the primary image, initrd retained for early tests).
- Superblock carries a clean flag cleared on mount and set on unmount so an unclean shutdown is reported at the next boot. A `fsck` pass for orphaned blocks is optional.
- Result: files written by the OS persist across reboots and are readable by `tools/mkfs --dump`.

## M14. Memory management extensions (completed 2026-08-29)
- `mmap` and `munmap` for anonymous memory, `brk` and `sbrk`.
- Swap: swap partition on a second virtio-blk device, swap slot bitmap, page eviction (clock algorithm over user anonymous pages), page table entries marked not present with the swap slot stored in the entry, fault handler swaps in.
- `procfs` style `/proc/meminfo` in devfs or a `meminfo` syscall for inspection.
- Result: a test program allocating more than physical memory completes.

## M15. Signals and process control (completed 2026-08-29)
- Signals: `SIGKILL`, `SIGTERM`, `SIGINT`, `SIGCHLD`, `SIGSEGV`, `SIGUSR1`, `SIGUSR2`, `kill`, `signal` or `sigaction`, delivery on return to user mode with a trampoline, default actions.
- Control C from the keyboard delivering `SIGINT` to the foreground process group.
- `ps` reading process information from a `/proc` like device file.
- `reboot` syscall (`RB_POWER_OFF`, `RB_AUTOBOOT`, `RB_HALT`) performing the orderly sequence: send `SIGTERM` to all processes except init, wait up to a timeout, send `SIGKILL`, flush the block cache and unmount filesystems marked clean, then call `power_off()` or `power_reboot()`. Halt prints a final message and stops the CPU with interrupts disabled.
- Init handles the shutdown request itself so that the syscall may only be issued by init, and other programs signal init (`SIGUSR1` for power off, `SIGUSR2` for reboot).
- Result: `kill` and control C terminate a running program.

## M16. User space applications (completed 2026-08-29)
- Coreutils: `ls`, `cat`, `echo`, `mkdir`, `rm`, `rmdir`, `cp`, `mv`, `touch`, `pwd`, `ps`, `kill`, `mount`, `sync`, `wc`, `head`, `hexdump`, `sleep`, `clear`, `shutdown`, `reboot`, `halt`.
- `shutdown` and `reboot` signal init as defined in M15. A shell builtin `exit` in the last shell does not shut the system down, it simply respawns via init.
- Shell improvements: quoting, environment variables, `export`, background jobs (`&`), exit status, script files.
- Text editor: a modal or nano style editor over the console with raw keyboard mode (a `termios` subset with `ICANON` and `ECHO` flags).
- Scripting interpreter: either a port of Lua 5.4 (requires `setjmp`, `longjmp`, `math`, `stdio` completeness in libc) or a small custom interpreter. Decide once libc coverage is known. Decision: the custom interpreter `mint`, because user programs cannot use floating point until FPU state is saved on context switches.

## M17. Windowing system (completed 2026-08-29)

Decisions (2026-08-29): the GUI comes before SMP; clients talk to the
window server through a dedicated syscall pair and shared memory
surfaces; widgets are drawn client side; the GUI starts on demand from
the shell with `startgui` and returns to the text console when the
server exits; the resolution is chosen on the kernel command line
(`video=WxH`, default 1024x768) and passed to Limine; GUI behaviour is
verified through a structured event log on serial plus CRC32 checksums
of framebuffer regions computed by a kernel test.

Stage 1, kernel input and output:
- PS/2 mouse driver on the second 8042 port (IRQ 12): packet decoding,
  movement and three buttons, `/dev/mouse` delivering fixed size events
  (dx, dy, buttons, timestamp) with a blocking read and a wait queue,
  `ISIG` unaffected. Keyboard events for the GUI come from `/dev/kbd` in
  raw mode with key press and release codes (a new `KBD_RAW_SCANCODES`
  flag), so the server can track modifiers itself.
- `/dev/fb0`: `ioctl` `FBIOGET_INFO` (width, height, pitch, bpp) and
  `mmap` of the framebuffer with write combining. `mmap` gains a file
  backed device mapping path (`file_ops.mmap`) used only by fb0 and by
  the shared memory objects below.
- Console handover: `ioctl FBIO_ACQUIRE` stops fbcon drawing while the
  server runs and `FBIO_RELEASE` (also on server exit through file
  release) restores the text console with a full redraw from the cell
  buffer.
- `video=WxH` on the command line, forwarded to Limine's `resolution`
  option by `tools/mkiso.sh`.

Stage 2, IPC:
- Shared memory objects: `shm_create(size)` returns a descriptor on an
  anonymous inode, `mmap` of it maps the same frames in both processes;
  reference counted, freed with the last mapping and descriptor.
  Descriptors are passed to another process by number through the
  message queue (a `shm_open(name)` namespace under `/dev/shm` is the
  fallback if descriptor passing proves awkward).
- Message queues: `mq_create(name)`, `mq_open(name)`, `mq_send(fd, buf,
  len)`, `mq_recv(fd, buf, len)` with fixed 256 byte messages, bounded
  depth, blocking receive interruptible by signals, and `poll`-style
  waiting through a `mq_wait(fds[], n)` call so the server can wait on
  the client queue, the mouse and the keyboard at once. Implemented in
  `ipc/mqueue.c` with its own lock recorded in `locking.md`.

Stage 3, window server (`user/wsrv/`):
- Owns fb0, the mouse and the keyboard; maintains a window list with z order,
  position, size, title, owning client and surface.
- Compositor with damage rectangles: only regions that changed are
  recomposed into a back buffer and copied to the framebuffer. Windows
  have a title bar with a close button, are moved by dragging the title
  bar, and are raised and focused by a click. A software cursor is
  drawn last.
- Protocol messages: `CREATE(width, height, title)` returning window id
  and surface descriptor, `DAMAGE(id, rect)`, `MOVE`, `RESIZE`, `SET_TITLE`,
  `DESTROY`; events to clients: `KEY(down/up, code, modifiers)`,
  `MOUSE(x, y, buttons)`, `FOCUS`, `CLOSE_REQUEST`, `RESIZED`.
- Serial event log (`wsrv: window 1 created 400x300 "term"`, `wsrv: focus
  1`, `wsrv: key 0x1e to 1`) used by the tests.
- `startgui` launches `wsrv` and one terminal window; `wsrv` exiting
  releases the framebuffer and control returns to the shell.

Stage 4, client library and toolkit (`libgui/`):
- `gui_connect`, `gui_window_create`, `gui_window_damage`, event loop
  with callbacks; drawing primitives on the surface (fill, rectangle,
  line, text with the 8x16 font, a second larger font generated by
  `tools/genfont`), clipping.
- Widgets drawn client side: label, button, text field, list box, a
  vertical layout box; keyboard focus inside a window; redraw by damage.

Stage 5, applications:
- Terminal emulator (`user/term/`): runs `sh` on a pseudo terminal pair
  (`/dev/pty` master and slave with the line discipline moved into a
  reusable `tty` layer shared with the console), VT100 subset matching
  fbcon, scrollback, selection not required.
- Clock, a file browser using the list box, and a text viewer, as
  demonstrations of the toolkit.

Tests:
- `mouse`: kernel test injecting PS/2 packets and checking `/dev/mouse`
  events.
- `fb0`: a user test mapping fb0, drawing a pattern, and the kernel
  checking a CRC32 of the region.
- `mq`: message queue and shared memory semantics between two processes.
- `gui`: kernel test that starts `wsrv` and the terminal, injects mouse
  movement, clicks and keystrokes, and checks the event log lines and
  framebuffer CRCs of the title bar and cursor regions.
- `gui_term`: types a command into the terminal window and checks that
  its output appears in the surface (CRC of the text rows).
- Result: `startgui` shows movable overlapping windows, a terminal that
  runs the shell, and the demonstration applications.

## M18. SMP (completed 2026-08-29)
- Application processor startup through the Limine MP protocol (the bootloader performs INIT and SIPI and parks the APs in long mode; the kernel moves them onto its own stacks and page tables before reclaiming bootloader memory), one `struct cpu` per processor, per CPU idle thread, per CPU GDT, TSS and APIC timer.
- Per CPU run queues behind the existing `sched_pick_next`; balancing by placing woken threads on idle CPUs and by stealing from other CPUs' queues (done).
- `tlb_flush_range` extended to send shootdown IPIs to CPUs that have the address space active, plus a drop request before an address space is freed (done, `mm/tlb.c`).
- Per CPU page caches in the buddy allocator and per CPU magazines in the slab allocator, both optional (deferred, done in M46).
- Lock debugging run with `CONFIG_LOCKDEBUG=1` (the default) across the whole suite with four CPUs (done).
- Files touched: `arch/x86_64/{smp,cpu,gdt,idt,apic,boot}.c`, `sched/mlfq.c`, `mm/{tlb,vmm}.c`, `sync/spinlock.c`, `drivers/timer.c`, `lib/klog.c`, `debug/panic.c`, plus the `nproc` and `getcpu` syscalls.
- Result: `nproc` reports the configured CPU count, `smptest` runs its children on every CPU with parallel speedup; all 35 cases pass with `-smp 4`.
