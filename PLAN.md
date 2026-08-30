# minios Development Plan

## 1. Design Summary

| Area | Decision |
|---|---|
| Architecture | x86_64, QEMU only (`qemu-system-x86_64`, `-M q35`, `-accel hvf` where available) |
| Language | C (C17, freestanding), NASM or GNU as for assembly stubs |
| Boot | Limine bootloader, Limine boot protocol, higher half kernel at `0xffffffff80000000` |
| Kernel type | Monolithic |
| CPUs | Boot CPU only at first. All per CPU state kept in a `struct cpu` reached via `GS` base so SMP can be added later without restructuring |
| Preemption | Timer interrupts trigger rescheduling only on return to user mode. Kernel code is not preempted |
| Execution model | Processes with multiple threads. Threads are the scheduling unit, processes own the address space, file table and thread list |
| Scheduler | Multilevel feedback queue (MLFQ) |
| Process creation | `fork` and `execve`, copy on write fork |
| Physical memory | Buddy allocator |
| Kernel heap | Slab allocator on top of the buddy allocator |
| Virtual memory | Copy on write, swapping of anonymous pages to a swap partition |
| Filesystem | Custom inode based filesystem (`mfs`) behind a VFS with mount points and devfs |
| Storage | virtio-blk (PCI, modern virtio interface) |
| System calls | POSIX subset, entered through `syscall` / `sysretq` |
| Executables | Static ELF64 |
| libc | Own minimal libc (`libc/`) |
| User space | Shell, coreutils, text editor, scripting interpreter |
| Console | Framebuffer text console with bitmap font, serial (COM1) mirror |
| Input | PS/2 keyboard |
| Timer | Local APIC timer calibrated against the PIT |
| Networking | None |
| Graphics | Text only initially. Windowing system as a late milestone |
| Privilege | Ring 3 from the first user process |
| Permissions | Single user, permission bits stored but not enforced |
| Testing | Automated QEMU boot tests checked via serial output, exit via `isa-debug-exit` |
| Debugging | GDB attached to QEMU gdbstub, kernel panic with symbolized backtrace |
| Build | Plain Makefiles |
| Version control | git, no CI |
| Reference | xv6 (structure and naming), Linux documentation for buddy, slab and MLFQ details |

## 2. Repository Layout

```
minios/
  Makefile                 top level: build all, image, run, test, gdb, clean
  toolchain.mk             compiler, flags, paths
  limine.conf              Limine configuration
  kernel/
    Makefile
    linker.ld              higher half layout, symbol table section
    arch/x86_64/           boot.c, gdt.c, idt.c, isr.S, paging.c, apic.c, pit.c, syscall.S, context.S, cpu.c
    include/               public kernel headers
    lib/                   string.c, printf.c, list.h, bitmap.c, kassert.c
    mm/                    pmm_buddy.c, vmm.c, slab.c, kmalloc.c, cow.c, swap.c, mmap.c
    sched/                 thread.c, proc.c, mlfq.c, wait.c
    sync/                  spinlock.c, mutex.c, semaphore.c, condvar.c
    ipc/                   pipe.c, signal.c
    fs/                    vfs.c, mount.c, file.c, devfs.c, mfs/ (superblock.c, inode.c, dir.c, bitmap.c), initrd.c
    drivers/               serial.c, fbcon.c, font.c, ps2kbd.c, pci.c, virtio/ (virtio.c, virtio_blk.c), debugexit.c
    syscall/               table.c, sys_proc.c, sys_fs.c, sys_mm.c, sys_misc.c
    debug/                 panic.c, backtrace.c, symbols.c
  libc/
    Makefile
    include/               stdio.h, stdlib.h, string.h, unistd.h, fcntl.h, sys/*.h, errno.h
    src/                   crt0.S, syscall.S, stdio/, stdlib/, string/, unistd/
  user/
    Makefile
    init/  sh/  coreutils/ (ls, cat, echo, mkdir, rm, cp, mv, ps, kill, mount, ...)  edit/  interp/  tests/
  tools/
    mkfs/                  host tool: builds an mfs disk image from a directory tree
    gensyms/               host tool: converts nm output into the kernel symbol table blob
  tests/
    run_qemu_test.sh       boots an image headless, captures serial, asserts on markers
    cases/                 one directory per boot test
  docs/
    design/                one document per subsystem, written when the subsystem is implemented
```

## 3. Milestones

Each milestone ends with a visible result and a boot test in `tests/cases/`. Milestones are ordered by dependency. Items within a milestone may be reordered.

### M0. Toolchain and build skeleton (completed 2026-08-29)
- `toolchain.mk` with `x86_64-elf-gcc`, `-ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mcmodel=kernel -mno-sse -mno-mmx -O2 -g`.
- Download Limine binaries into `third_party/limine/`, `limine.conf` with a single entry.
- `linker.ld` for a higher half kernel with `.text`, `.rodata`, `.data`, `.bss`, `.ksyms` sections, 4 KiB aligned.
- `make image` produces `build/minios.iso` (initial) and later `build/disk.img`.
- `make run` starts QEMU with `-serial stdio -m 512M -M q35`.
- `make gdb` starts QEMU with `-s -S` and prints the GDB command line. `.gdbinit` loads symbols and connects.
- `make test` runs `tests/run_qemu_test.sh` against every case.
- Result: kernel entry reached, "minios booting" printed on serial.

### M1. Early kernel and console (completed 2026-08-29)
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

### M2. Debugging infrastructure (completed 2026-08-29)
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

### M3. Physical memory: buddy allocator (completed 2026-08-29)
- Parse the Limine memory map. Use the Limine HHDM (higher half direct map) for physical access.
- Buddy allocator with orders 0 to 10 (4 KiB to 4 MiB), one free list per order, per page `struct page` array with order and flags.
- `pmm_alloc(order)`, `pmm_free(page, order)`, `pmm_alloc_page()`, statistics dump.
- Free lists protected by a single `pmm_lock` spinlock from the first version. Per CPU page caches can be added in M17 without changing the interface.
- Boot test: allocate and free patterns, verify coalescing by checking free counts.

### M4. Virtual memory (completed 2026-08-29)
- Own page tables (PML4) replacing Limine's, kernel mapped in the higher half, HHDM preserved, framebuffer mapped write combining.
- `vmm_map`, `vmm_unmap`, `vmm_protect`, `vmm_translate`, address space creation and destruction.
- Page fault handler reporting faulting address (`CR2`), error code and instruction pointer.
- Each address space carries its own lock. Kernel page table modifications go through a single `kvm_lock`. Every unmap and protect call goes through `tlb_flush_range`, which flushes locally now and becomes the IPI shootdown entry point in M17, so no caller needs to change.
- Kernel stack guard pages.
- Result: page faults on unmapped kernel addresses are reported correctly.

### M5. Kernel heap: slab allocator (completed 2026-08-29)
- Slab caches with fixed object sizes, `kmem_cache_create`, `kmem_cache_alloc`, `kmem_cache_free`, slabs backed by buddy pages, free list within each slab.
- `kmalloc` and `kfree` built on size class caches (16 B to 8 KiB), larger requests fall through to the buddy allocator.
- Poisoning on free in debug builds, redzone check.
- One spinlock per cache. Per CPU magazines are an optional M17 addition behind the same `kmem_cache_alloc` interface.
- Boot test: allocation stress with pattern verification.

### M6. Interrupts and timing (completed 2026-08-29)
- Local APIC enabling, PIC disabling, spurious interrupt vector.
- I/O APIC redirection for the PS/2 keyboard and PIT.
- PIT used once to calibrate the APIC timer frequency, then APIC timer configured at 1000 Hz.
- `sleep_ms` busy variant, monotonic tick counter.
- PS/2 keyboard driver with scancode set 1 to keycode translation, shift and control handling, line discipline buffer.
- Result: keys typed in QEMU are echoed to the console.

### M7. Kernel threads and scheduler (completed 2026-08-29)
- `struct thread` (kernel stack, saved context, state, priority level, time slice remaining, owning process) and `struct proc` (address space, thread list, pid, parent, file table, cwd, exit status).
- Context switch in assembly (`context.S`), saving callee saved registers and `rsp`.
- MLFQ: 8 queues, time slice doubling per level (10 ms at the top), demotion on slice exhaustion, promotion on block, periodic priority boost every 1 s to prevent starvation.
- `mutex`, `semaphore`, `condvar` built on a wait queue, each internally protected by a spinlock from M2. Blocking primitives are never used from interrupt context.
- Run queues protected by `sched_lock`. The scheduler reads `cpu_current()->thread`, never a global. Per CPU run queues in M17 replace the single queue behind `sched_pick_next` without changing callers.
- Idle thread, `thread_create`, `thread_exit`, `yield`, `sleep_ms` blocking variant.
- Result: several kernel threads print interleaved output at different priorities.

### M8. User mode and system calls (completed 2026-08-29)
- User code segment, `syscall` MSR setup (`STAR`, `LSTAR`, `SFMASK`), `syscall.S` entry saving user state, swapping to the kernel stack via the per CPU area and `swapgs`.
- Syscall table with numbers in `kernel/include/syscall_nums.h`, shared with libc.
- Initial syscalls: `write`, `exit`, `getpid`, `yield`.
- First user program embedded in the kernel image as a flat binary, then replaced by ELF loading in M9.
- Result: a ring 3 program calls `write` and `exit`.

### M9. ELF loader, initrd, fork and exec (completed 2026-08-29)
- Initrd (tar or cpio) loaded by Limine as a module, exposed as a read only filesystem.
- ELF64 loader: program headers, `PT_LOAD` mapping, stack setup with `argc`, `argv`, `envp`.
- `fork` with copy on write: page table copy marking writable pages read only and tagged COW, per page reference count in `struct page`, fault handler copies on write.
- `execve`, `wait4`, `exit`, `kill` (terminate only), `getppid`.
- User threads: `thread_create` syscall (`clone` style with a new stack), `thread_exit`, `thread_join`.
- Result: `init` forks and executes a second program from the initrd.

### M10. libc and the first user programs (completed 2026-08-29)
- `crt0.S`, syscall wrappers, `errno`.
- `string.h` complete, `stdio.h` (`printf`, `fprintf`, `snprintf`, `puts`, `getchar`, `fgets`, buffered `FILE`), `stdlib.h` (`malloc` via `sbrk` or `mmap`, `atoi`, `strtol`, `exit`, `abort`), `unistd.h`, `fcntl.h`, `dirent.h`, `sys/wait.h`, `sys/stat.h`, `errno.h`, `assert.h`, `ctype.h`.
- `init` and a minimal `sh` (command line parsing, `PATH` lookup, `fork`/`exec`/`wait`, `cd`, `exit`).
- Result: interactive shell running programs from the initrd.

### M11. VFS (completed 2026-08-29)
- `struct inode`, `struct dentry` (or a simple path cache), `struct file`, `struct fs_ops`, `struct inode_ops`, `struct file_ops`.
- Path resolution with `.` and `..`, relative to cwd, mount point crossing.
- Mount table, `mount`, `umount`, root mount of the initrd.
- devfs with `/dev/console`, `/dev/null`, `/dev/zero`, `/dev/kbd`, later `/dev/vda`.
- File descriptor table per process, `open`, `close`, `read`, `write`, `lseek`, `dup`, `dup2`, `stat`, `fstat`, `getdents`, `mkdir`, `unlink`, `rmdir`, `chdir`, `getcwd`, `rename`.
- Pipes and `pipe`, shell redirection (`<`, `>`, `|`).
- Result: `ls /dev`, `cat file | wc` work.

### M12. PCI and virtio-blk (completed 2026-08-29)
- PCI enumeration over the configuration space, device listing on boot.
- virtio over PCI (modern interface), virtqueue implementation, virtio-blk driver with interrupt driven completion.
- Block layer: `struct blockdev`, request queue, block cache (write back, LRU, `sync` syscall).
- `/dev/vda` in devfs.
- Result: raw sector reads and writes through the block cache.

### M13. mfs filesystem (completed 2026-08-29)
- On disk layout: superblock, inode bitmap, block bitmap, inode table, data blocks. Inodes with 12 direct, one indirect and one double indirect pointer. Directories as arrays of fixed size entries. Block size 4 KiB.
- `tools/mkfs` host tool building an image from a directory tree.
- Kernel implementation: superblock reading, inode read and write, block allocation, directory lookup, create, unlink, truncate, rename, hard links.
- Root switched from initrd to `/dev/vda` (`disk.img` becomes the primary image, initrd retained for early tests).
- Superblock carries a clean flag cleared on mount and set on unmount so an unclean shutdown is reported at the next boot. A `fsck` pass for orphaned blocks is optional.
- Result: files written by the OS persist across reboots and are readable by `tools/mkfs --dump`.

### M14. Memory management extensions (completed 2026-08-29)
- `mmap` and `munmap` for anonymous memory, `brk` and `sbrk`.
- Swap: swap partition on a second virtio-blk device, swap slot bitmap, page eviction (clock algorithm over user anonymous pages), page table entries marked not present with the swap slot stored in the entry, fault handler swaps in.
- `procfs` style `/proc/meminfo` in devfs or a `meminfo` syscall for inspection.
- Result: a test program allocating more than physical memory completes.

### M15. Signals and process control (completed 2026-08-29)
- Signals: `SIGKILL`, `SIGTERM`, `SIGINT`, `SIGCHLD`, `SIGSEGV`, `SIGUSR1`, `SIGUSR2`, `kill`, `signal` or `sigaction`, delivery on return to user mode with a trampoline, default actions.
- Control C from the keyboard delivering `SIGINT` to the foreground process group.
- `ps` reading process information from a `/proc` like device file.
- `reboot` syscall (`RB_POWER_OFF`, `RB_AUTOBOOT`, `RB_HALT`) performing the orderly sequence: send `SIGTERM` to all processes except init, wait up to a timeout, send `SIGKILL`, flush the block cache and unmount filesystems marked clean, then call `power_off()` or `power_reboot()`. Halt prints a final message and stops the CPU with interrupts disabled.
- Init handles the shutdown request itself so that the syscall may only be issued by init, and other programs signal init (`SIGUSR1` for power off, `SIGUSR2` for reboot).
- Result: `kill` and control C terminate a running program.

### M16. User space applications (completed 2026-08-29)
- Coreutils: `ls`, `cat`, `echo`, `mkdir`, `rm`, `rmdir`, `cp`, `mv`, `touch`, `pwd`, `ps`, `kill`, `mount`, `sync`, `wc`, `head`, `hexdump`, `sleep`, `clear`, `shutdown`, `reboot`, `halt`.
- `shutdown` and `reboot` signal init as defined in M15. A shell builtin `exit` in the last shell does not shut the system down, it simply respawns via init.
- Shell improvements: quoting, environment variables, `export`, background jobs (`&`), exit status, script files.
- Text editor: a modal or nano style editor over the console with raw keyboard mode (a `termios` subset with `ICANON` and `ECHO` flags).
- Scripting interpreter: either a port of Lua 5.4 (requires `setjmp`, `longjmp`, `math`, `stdio` completeness in libc) or a small custom interpreter. Decide once libc coverage is known. Decision: the custom interpreter `mint`, because user programs cannot use floating point until FPU state is saved on context switches.

### M17. Windowing system (completed 2026-08-29)

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
- Owns fb0, the mouse and the keyboard; keeps a window list with z order,
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

### M18. SMP (completed 2026-08-29)
- Application processor startup through the Limine MP protocol (the bootloader performs INIT and SIPI and parks the APs in long mode; the kernel moves them onto its own stacks and page tables before reclaiming bootloader memory), one `struct cpu` per processor, per CPU idle thread, per CPU GDT, TSS and APIC timer.
- Per CPU run queues behind the existing `sched_pick_next`; balancing by placing woken threads on idle CPUs and by stealing from other CPUs' queues (done).
- `tlb_flush_range` extended to send shootdown IPIs to CPUs that have the address space active, plus a drop request before an address space is freed (done, `mm/tlb.c`).
- Per CPU page caches in the buddy allocator and per CPU magazines in the slab allocator, both optional (not done).
- Lock debugging run with `CONFIG_LOCKDEBUG=1` (the default) across the whole suite with four CPUs (done).
- Files touched: `arch/x86_64/{smp,cpu,gdt,idt,apic,boot}.c`, `sched/mlfq.c`, `mm/{tlb,vmm}.c`, `sync/spinlock.c`, `drivers/timer.c`, `lib/klog.c`, `debug/panic.c`, plus the `nproc` and `getcpu` syscalls.
- Result: `nproc` reports the configured CPU count, `smptest` runs its children on every CPU with parallel speedup; all 35 cases pass with `-smp 4`.

### M19. Windowing system enhancements (completed 2026-08-30)

Decisions (2026-08-30): the GUI stays on demand through `startgui`; the
protocol keeps 256 byte messages while the window and client tables
become dynamic; verification continues with the serial event log plus
CRC32 and pixel checks of framebuffer regions; the work forms one
milestone with one design document (`docs/design/gui.md` extended) and
one boot test per stage.

Stage 1, kernel input and output (done 2026-08-30):
- Mouse scroll wheel: the IntelliMouse enable sequence (sample rate 200,
  100, 80, then device id 3), four byte packets, a `dz` field in
  `struct mouse_event`. Devices that stay at id 0 keep three byte packets.
- Framebuffer format: `FBIOGET_INFO` gains the red, green and blue mask
  sizes and shifts taken from the Limine framebuffer response. `fbcon`
  and `wsrv` compose pixels through the reported layout, so 24 bit and
  BGR modes work. 32 bit RGB remains the fast path.
- Window size changes on pseudo terminals already exist through
  `TIOCSWINSZ`; the slave gets `SIGWINCH` to its foreground group.

Stage 2, protocol and server (`user/wsrv/`, done 2026-08-30):
- `WM_RESIZE(id, w, h)` from clients and `WM_RESIZED(w, h, generation)`
  to clients: the server allocates a new surface
  `wsrv.s<id>.<generation>` and unlinks the old one at once (it lives
  until the client unmaps it); the client library maps the new one
  before the application sees the event. Windows carry minimum sizes.
- Decorations: a resize handle in the bottom right corner, maximize and
  minimize boxes in the title bar. Maximize fills the desktop area above
  the task bar and remembers the previous frame. Minimized windows are
  hidden and listed in the task bar. Windows are clamped so that their
  title bar stays on screen.
- Server keyboard shortcuts: Alt+Tab cycles focus in z order, Alt+F4
  sends `WM_CLOSE`, Alt+drag anywhere in a window moves it.
- Clipboard: contents always go through the shared memory object
  `wsrv.clip` (64 KiB); `WM_CLIP_SET(len)` and `WM_CLIP_GET` returning
  `WM_CLIP_DATA(len)`. Only text is supported.
- Task bar drawn by the server at the bottom of the screen: one button
  per window showing the title (click focuses or restores), a launcher
  menu listing programs from `/etc/launcher` (one `title=path` line per
  entry), and the clock.
- Compositor: occlusion culling skips windows fully covered by opaque
  windows above them within a damage rectangle, and clips each window's
  drawing to its visible region computed by subtracting the frames of
  windows above it. Dynamic window and client arrays replace the fixed
  tables.
- Wheel events reach the window under the cursor as `WM_MOUSE` with
  kind `WMOUSE_WHEEL` and the delta in `d`.

Stage 3, client library and toolkit (`libgui/`, done 2026-08-30):
- Fonts: `tools/genfont/genfont.py` gains a mode producing a variable
  width font file (`.mfnt`: header, glyph advances, bitmaps) from a
  larger source face, built into `user/etc/fonts/`. `gfx` loads fonts
  from the filesystem (`gfx_font_load`) and draws with either font; the
  built in 8x16 font stays the default and the fallback.
- Widgets: scroll bar, check box, text area with multi line editing,
  menu bar with drop down menus, modal message dialog. Text fields and
  areas support selection, and Ctrl+C, Ctrl+V and Ctrl+X through the
  clipboard. Widgets react to wheel events (list box, text area, scroll
  bar).
- Layout on resize: `ui_run` handles `WM_RESIZED` by remapping the
  surface and relayouting the tree. Boxes support minimum sizes.

Stage 4, applications (done 2026-08-30):
- Terminal: cell grid derived from the window size, resize propagates
  through `TIOCSWINSZ`, a 1000 line scrollback scrolled with the wheel
  and Shift+PageUp/PageDown, the larger font when available.
- `files` and `view` use the scroll bar and the resize path; `view`
  gains a menu bar with open and font size entries. A `launcher` entry
  file lists `term`, `files`, `view`, `paint`, `pong`, `clock`.

Stage 5, double buffering (done 2026-08-30): applications draw into a
private buffer; `gui_damage` copies the rectangle into the shared
surface, so the compositor never shows a half drawn frame. The server
already composes into its own back buffer before copying to the
framebuffer.

Tests:
- `mouse_wheel`: injected four byte packets produce `dz` events, three
  byte devices keep working.
- `fb_format`: the kernel reports the mask layout and a user program
  draws a known color that the kernel verifies in the native format
  (run with a 24 bit mode in addition to the default).
- `gui_resize`: a client resizes and maximizes a window, the server log
  shows the new geometry and the CRC of the redrawn frame matches.
- `gui_wm`: Alt+Tab focus order, minimize to the task bar and restore
  by clicking the task bar button, occlusion (a window fully covered
  is not drawn, checked by a counter in the server log).
- `gui_clip`: two clients exchange text through the clipboard, checked
  through the client logs.
- `gui_term`: extended with a resize of the terminal window followed by
  a `stty size` style query in the shell and a scrollback check.
- Result: `startgui` shows a task bar with a launcher, resizable
  windows with maximize and minimize, a terminal that resizes and
  scrolls back, and text copied between applications.

### M20. Font rendering (completed 2026-08-30)

Decisions (2026-08-30): outline font rendering lives in its own library
`libfont/` (`libfont.a`), independent from the window server and the
toolkit; libgui consumes it through the existing `struct font` so every
widget and application can switch to an outline font without changes.
User programs still run without FPU state saving, so the rasterizer and
all transforms use fixed point integer arithmetic. Test fonts are the
DejaVu faces (TrueType outlines, `kern` and `GPOS` tables) and Latin
Modern Roman (CFF outlines, `GPOS` kerning) from `third_party/`.

Stage 1, parser (`libfont/src/ttf.c`, `cff.c`):
- `font_open(path)` maps the file, reads the table directory of TrueType
  (`\0\1\0\0`, `true`) and OpenType (`OTTO`) files, and the `head`,
  `hhea`, `hmtx`, `maxp`, `cmap` (formats 4 and 12, format 0 as a
  fallback), `loca`, `glyf`, `CFF `, `kern` and `GPOS` tables.
- Glyph lookup by Unicode code point; advance width and left side
  bearing in font units; ascent, descent, line gap, units per em.
- Outlines as contours of points in font units: TrueType quadratic
  curves from `glyf` including composite glyphs with offsets and
  scales; CFF Type 2 charstrings (moveto, lineto, curveto families,
  hstem/vstem/hintmask skipping, subroutines with bias, endchar,
  the width prefix) with cubic curves.

Stage 2, rasterizer (`libfont/src/raster.c`):
- Outlines are scaled to a pixel size in 26.6 fixed point, curves are
  flattened by fixed subdivision, and edges are rasterized into an 8 bit
  coverage bitmap with non zero winding: 4 sub scanlines per pixel row
  and exact horizontal coverage between crossings, accumulated in a per
  row buffer. Output is a glyph bitmap with bearing and advance in
  pixels (advance in 26.6 for subpixel positioning of the pen).
- A glyph cache keyed by font, size and glyph id keeps the last few
  hundred bitmaps.

Stage 3, kerning and shaping (`libfont/src/kern.c`):
- `kern` table format 0 pairs and `GPOS` pair adjustment (lookup type
  2, formats 1 and 2 with class definitions, through extension lookups)
  for the `kern` feature; `font_kern(font, left, right)` in font units.
- `font_shape(font, text, size)` walks a string, applies kerning to the
  pen, and returns glyph positions in 26.6 pixels.

Stage 4, integration (`libgui/`):
- `gfx_font_open_ttf(path, px)` returns a `struct font` backed by an
  outline font; `gfx_text_font` blends coverage bitmaps into the surface
  (alpha over a solid colour or the surface contents), `gfx_text_width_font`
  and `gfx_text_index_font` use shaped positions, so widgets, `term` and
  `view` work unchanged. `view` gains an "Outline font" menu entry, the
  launcher font list in `/etc/fonts/` holds the three test fonts.

Tests:
- `ttf`: `/bin/fonttest` parses both fonts and checks units per em,
  glyph counts, glyph ids, advances, kerning pairs (`AV` from `kern` and
  from `GPOS`, `To` from Latin Modern's `GPOS`), outline bounds of `A`,
  and rasterizes `A` at 32 pixels: the bitmap fits the scaled bounds,
  contains fully covered and partially covered pixels, and the sum of
  coverage matches the glyph's area within tolerance.
- `gui_ttf`: a window draws a string with the outline font; pixels are
  checked for intermediate colours (antialiasing) against a plain
  background and a CRC of the text row is logged.
- Result: applications render antialiased, kerned TrueType and CFF
  OpenType text at any size.

### M21. Application framework core (completed 2026-08-30)

Decisions (2026-08-30, from sixteen questions): a retained widget tree
with typed events delivered through signals; one application object
driving every window, timer and extra descriptor; boxes plus a grid
container with size hints; a theme structure with named colours and
metrics; per widget invalidation with clipped partial redraws; the
default interface font is DejaVu Sans at 14 pixels rendered by libfont
with a global scale factor; widgets such as tabs, split panes, tool
bars, status bars, combo boxes, spinners, sliders, progress bars, radio
buttons, tree views, tables, image views and icons; PNG images through
an own inflate; a text editor widget with undo, word wrap and syntax
colouring; drag and drop between applications through the server;
interfaces described in a declarative text file loaded at runtime;
menus, tooltips and popups as undecorated override windows; per window
alpha in the compositor; boot tests plus a host unit test build. The
work is split into M21 (core), M22 (widgets, images, editor) and M23
(interface files, popups, drag and drop, compositor alpha). The M19
toolkit (`gui/widgets.h`) stays available until every application has
moved to the framework in M22, then it is removed.

Stage 1, application object (`libgui/src/app.c`, `gui/app.h`):
- `app_create`, `app_run`, `app_quit`; windows registered with the
  application; one `poll` loop over the event queue, timers
  (`app_timer_add(ms, repeat, cb)`, ordered by deadline) and application
  descriptors (`app_watch_fd(fd, events, cb)`), so `term` no longer needs
  its own loop. Events are demultiplexed to windows by id; `WM_RESIZED`,
  focus and close are turned into signals on the window widget.
- Idle work: redraws happen once per loop iteration after all pending
  events are handled, so bursts of input cost one paint.

Stage 2, widget object model (`libgui/src/widget.c`, `gui/widget.h`):
- `struct widget` with a class pointer (`struct widget_class`: name,
  size, `measure`, `layout`, `paint`, `event`, `destroy`), a parent and
  children list, geometry, flags (visible, enabled, focusable, dirty),
  properties (`widget_set_int`, `widget_set_text`, `widget_get_*`) and
  an id string for lookup (`widget_find(root, "ok")`).
- Signals: `widget_connect(w, "clicked", handler, arg)` where each
  signal name has a fixed argument structure (`struct sig_click`,
  `struct sig_change`, `struct sig_key`, ...); handlers return whether
  the event is consumed; several handlers per signal run in order.
- Focus traversal with Tab and Shift+Tab, keyboard accelerators
  (`widget_set_accel(w, key, mods)`), mnemonics in captions.

Stage 3, layout (`libgui/src/layout.c`):
- Size hints: minimum, preferred and maximum width and height computed
  by `measure`, stretch factors, alignment inside the cell, margins.
- Containers: `box` (horizontal or vertical, spacing, padding) and
  `grid` (rows and columns, spans, per row and column stretch). Layout
  runs top down after measurement bottom up; only subtrees marked for
  relayout are recomputed.

Stage 4, theme and painting (`libgui/src/theme.c`, `paint.c`):
- `struct theme` with named colours (window, text, disabled text,
  field, selection, accent, borders, highlights) and metrics (padding,
  spacing, border width, corner radius, scroll bar width, font, font
  size); a default theme compiled in and `theme_set` at runtime;
  a global scale factor applied to metrics and font size.
- Painting through a `struct painter` bound to a surface with a clip
  rectangle stack and an origin, so widgets paint in local coordinates
  and cannot draw outside their area; primitives: fill, frame, line,
  text with the theme font, image blit, rounded rectangles, focus ring.
- Partial redraw: `widget_invalidate` marks a widget and its ancestors;
  the paint pass repaints only dirty widgets (and the descendants they
  overlap), unions their rectangles and sends one `gui_damage` per
  window.

Stage 5, core widgets on the new model (`libgui/src/widgets/*.c`):
- label, button, check box, radio button group, text field, list view,
  scroll bar, scroll area (viewport with two scroll bars), canvas,
  separator, box, grid, window. These replace the M19 equivalents in
  behaviour and add the signal interface.

Stage 6, host unit tests (`libgui/tests/`):
- `make check` builds libgui and libfont with the host compiler against
  a fake surface and a scripted event source (`tests/harness.c`), and
  runs unit tests of layout (measured sizes, grid spans, stretch),
  signals (order, consumption), partial redraw (which rectangles are
  damaged), focus traversal and text editing.

Tests:
- `make check` on the host as above.
- `gui_app`: a boot test client with two windows, a timer and a watched
  pipe descriptor; the kernel injects input and checks the log lines
  and pixels, including that a single change repaints only its widget
  (damage rectangles logged by the client).
- Existing GUI cases continue to pass with the M19 toolkit in place.
- Pipes gained a `poll` operation so watched pipe descriptors block
  correctly (`kernel/ipc/pipe.c`).
- Result: applications are built from a retained tree with signals, an
  application loop, themed painting and partial redraws. Design notes
  in `docs/design/framework.md`.

### M22. Widgets, images and the text editor (completed 2026-08-30)

Stage 1, images (`libgui/src/png.c`, `image.c`):
- A deflate decoder (stored, fixed and dynamic Huffman blocks), zlib
  framing with Adler-32, PNG chunks (IHDR, PLTE, tRNS, IDAT, IEND),
  filters (none, sub, up, average, Paeth), colour types grey, RGB,
  palette, grey with alpha and RGBA at 8 bits, interlacing rejected.
  `image_load(path)` returns an RGBA `struct image`; `painter_image`
  blends with alpha. Icons for the launcher, buttons and menus from
  `/usr/share/icons/*.png`, generated at build time from simple sources.

Stage 2, controls: combo box (popup list, editable option), spinner,
slider, progress bar, tabs, split pane (draggable divider), tool bar
with icon buttons, status bar with sections, tooltip text on any widget.

Stage 3, data views: `struct model` interfaces for lists, trees and
tables (row count, cell text, children, expansion); tree view with
expanders and indentation, table with resizable and sortable columns,
both virtualised over the model so large data is not copied; selection
signals.

Stage 4, text editor widget (`libgui/src/editor.c`): a line array with
per line strings, an undo and redo stack of edit operations, word wrap
mode, line numbers, search, a highlighter interface with C and shell
highlighters supplied, and the clipboard. `edit` gains a graphical
front end `gedit` built on it.

Stage 5, application migration: `term`, `files`, `view`, `clock`,
`paint`, `pong`, `widgettest` and the launcher move to the framework;
`gui/widgets.h` and the M19 toolkit files are removed.

Tests: `make check` extended with PNG decoding (a known image compared
pixel by pixel, filter and colour type coverage), the deflate decoder
against stored and dynamic blocks, model views (row virtualisation) and
editor operations (undo and redo, wrap positions). Boot tests
`gui_controls` (combo box, slider, tabs, split pane through injected
input) and `gui_editor` (typing, undo, wrap, highlighting colours in
pixels). Result: a complete widget set with images and an editor.

### Display server rework (M23 to M26)

Decisions (2026-08-30): the message queue based `wsrv` is replaced by
a Wayland inspired design of our own (own wire format, no `wl_` or
`xdg_` names): Unix domain sockets with descriptor passing as the
transport, shared memory pools with attach, commit and buffer release,
a 60 Hz frame clock with frame callbacks, toplevel and popup roles with
the panel as a separate layer client, server side decorations that a
client can decline, a data device model for clipboard and drag and
drop, keymaps and key codes sent to clients which translate them,
protocol definitions in XML with a Python scanner generating the C
marshalling code, a new compositor and client library with the
applications ported through the unchanged libgui framework API. The
former M23 entry (interface files, popups, drag and drop) is
superseded: popups and drag and drop are part of M25 and M26, the
`.ui` builder is an optional later milestone.

### M23. Kernel IPC and libc (completed 2026-08-30)

New syscalls, numbered after `SYS_uname 54` in
`kernel/include/syscall_nums.h`, entries in `kernel/syscall/table.c`,
prototypes in `kernel/include/syscall/syscalls.h`, implementations in
`kernel/syscall/sys_ipc.c` and `sys_fs.c`, libc stubs through
`syscallN` (`libc/include/minios/syscall.h`), new headers
`libc/include/sys/socket.h`, `sys/un.h`, `sys/eventfd.h`,
`sys/timerfd.h`, additions to `fcntl.h`, `unistd.h`, `sys/mman.h`.

1. Unix domain stream sockets (`kernel/ipc/socket.c`): `socket(AF_UNIX,
   SOCK_STREAM, flags)`, `socketpair`, `bind` and `connect` on an
   abstract name (a kernel table of listening names, `SOCK_NAME_MAX
   32`; no filesystem inode), `listen`, `accept`, `shutdown`. A
   connection is two 64 KiB rings (one per direction) with the bounce
   buffer discipline of `pipe.c`; reads and writes block unless the
   descriptor is non blocking (`-EAGAIN`); `poll` reports `POLLIN`,
   `POLLOUT` and the new `POLLHUP`; peer close gives end of file and
   `-EPIPE`.
2. Descriptor passing: `sendmsg` and `recvmsg` with `struct msghdr`,
   `struct iovec` and `SCM_RIGHTS` control messages (at most 16
   descriptors per message). Passed files are queued in the socket as
   (byte offset, file list) records and delivered with the first byte
   of their message, as on Linux; unread files are released when the
   socket closes.
3. `memfd_create(name, flags)` and `ftruncate`: an unnamed shm object
   (`kernel/ipc/shm.c` gains a constructor without a name and a grow
   operation) that is passed over sockets and mapped with `mmap`.
4. `eventfd(initval, flags)` (64 bit counter, read blocks on zero or
   returns `-EAGAIN`, write adds, `poll`) and `timerfd_create`,
   `timerfd_settime`, `timerfd_gettime` (`kernel/ipc/eventfd.c`,
   `timerfd.c`; expiration counter driven by the timer subsystem, read
   returns and clears it).
5. Descriptor flags: `struct file` gains `nonblock`; `struct fdtable`
   gains a close on exec bit map; `fcntl(F_GETFL, F_SETFL, F_GETFD,
   F_SETFD, F_DUPFD, F_DUPFD_CLOEXEC)`; `O_NONBLOCK` and `O_CLOEXEC`
   accepted by `open`, `socket`, `socketpair`, `pipe2`, `eventfd`,
   `timerfd_create`, `memfd_create`; `execve` closes marked
   descriptors. Non blocking mode honoured by sockets, pipes, eventfd,
   timerfd and message queues.
6. `poll`: limit raised to 64 descriptors, `POLLHUP` and `POLLERR`
   defined in `minios/abi.h`, and a positive timeout waits on the poll
   wait queue with a deadline instead of the current 5 ms busy loop
   (`poll_files` in `kernel/ipc/mqueue.c:240`).
7. FPU state: `struct thread` gains a 512 byte 16 byte aligned area;
   `fxsave` on switch out and `fxrstor` on switch in
   (`kernel/sched/mlfq.c` around `context_switch`), initial state from
   `fninit` and a default `MXCSR`, CR4 `OSFXSR` and `OSXMMEXCPT` set in
   `cpu_init`, the signal frame saves and restores the area, and the
   user flags in `toolchain.mk` drop `-mno-sse -mno-sse2 -mno-80387`
   (the kernel keeps them). libc `memcpy` may then use 16 byte moves.
8. Documents: `docs/design/sockets.md` (sockets, descriptor passing,
   memfd, eventfd, timerfd, descriptor flags) and an FPU section in
   `docs/design/scheduler.md`; lock ordering in `docs/design/locking.md`.

Tests (`tests/cases/`): `sockets` (`/bin/socktest`: socketpair echo,
abstract bind, listen, accept and connect across fork, descriptor
passing of a memfd and a pipe end with contents verified on the
receiving side, `POLLHUP` on peer close, `-EAGAIN` in non blocking
mode, `-EPIPE`), `evfd` (eventfd counter semantics, timerfd periodic
expirations counted against `uptime_ms`, both under `poll`), `fdflags`
(close on exec across `execve`, `F_DUPFD_CLOEXEC`, `O_NONBLOCK` on a
pipe), `fpu` (two user threads and a forked child doing SSE
arithmetic in loops while the other runs, results checked; a signal
handler clobbering SSE registers), plus the poll timeout accuracy in
the existing `mq` case.

### M24. Protocol library, scanner and compositor core (completed 2026-08-30)

1. Protocol definition `protocol/core.xml`: interfaces `display`
   (sync, get_registry, error and delete_id events), `registry` (global,
   global_remove, bind), `callback` (done), `compositor`
   (create_surface), `shm` (create_pool, format event), `shm_pool`
   (create_buffer, resize, destroy), `buffer` (release event, destroy),
   `surface` (attach, damage, frame, commit, set_opaque_region as a
   rectangle list, destroy), `output` (geometry and mode events).
   Argument types: int, uint, fixed (24.8), string, array, object,
   new_id, fd. Message header: object id (u32), opcode (u16), size
   (u16); client ids from 1, server ids from 0xff000000; descriptors
   travel in `SCM_RIGHTS` control messages in the order the fd
   arguments appear.
2. Scanner `tools/wscan/wscan.py` (Python, no dependencies) writes
   `libwire/generated/core-client.h/.c` and `core-server.h/.c`:
   per interface a request function set (client) or listener structure
   (server), event listener structures (client) or send functions
   (server), interface descriptors with argument signatures for the
   generic marshaller. Generated code is committed.
3. Library `libwire/` (`libwire.a`, also compiled on the host for unit
   tests): connection with output buffer and descriptor queue, flush,
   read and dispatch, client proxies with listeners and user data,
   server resources with dispatch tables and destruction callbacks, id
   allocation and `delete_id`, roundtrip through `display.sync`,
   protocol error reporting that closes the connection.
4. Compositor `user/compositor/`: `main.c` (event loop over the listen
   socket, client sockets, input descriptors and the frame timerfd),
   `client.c` (connections, registry, globals), `surface.c` (pending
   and current state, attach, damage in surface coordinates, commit,
   buffer release when a newer buffer is committed or the surface is
   hidden), `shm.c` (pools mapped from passed memfds, buffer format
   `XRGB8888` and `ARGB8888` with per surface alpha), `scene.c` (the
   scene of surfaces with z order, damage merging, occlusion culling
   and per rectangle composition carried over from `wsrv/compose.c`),
   `backend_fb.c` (framebuffer mapping and format conversion from
   `wsrv/compose.c`), `input.c` (mouse and keyboard reading from the
   devices, carried over from `wsrv/input.c`, raw at this stage).
   Composition runs once per 16 ms frame tick when damage exists;
   `callback.done` carries the frame time in ms.
5. `user/compositor` logs `comp: ...` lines for the tests (client
   connected, surface created, buffer attached, frame, buffer
   released, slow frame).
6. Document `docs/design/protocol.md` (wire format, object model,
   scanner, library) and `docs/design/compositor.md` (core state
   machine, frame clock, buffer lifecycle).

Tests: `make check` gains `libwire/tests/` on the host (marshal and
unmarshal every argument type, descriptor passing over a host
socketpair, id allocation, error paths); boot test `comp_core`
(`/bin/comptest core`: connects, binds globals, creates a pool with two
buffers, draws, attaches, damages, commits, waits for `callback.done`,
commits the second buffer and receives `buffer.release` for the first;
the kernel checks pixels on the framebuffer and the log).

### M25. Shell, seat, data device and panel (completed 2026-08-30)

1. `protocol/shell.xml`: `shell` (get_toplevel, get_popup,
   create_positioner, get_layer_surface), `shell_surface` base
   (configure event with serial, ack_configure), `toplevel` (set_title,
   set_app_id, set_min_size, set_max_size, move, resize with edges,
   set_maximized, unset_maximized, set_minimized, close event,
   configure with width, height and a state array), `popup` (grab,
   done event, reposition), `positioner` (anchor rectangle, gravity,
   offset, constraint adjustment), `layer_surface` (anchor edges,
   exclusive zone, size, keyboard interactivity), `decoration`
   (set_mode server or client, mode event), `toplevel_manager` for the
   panel (toplevel list events: title, app_id, state; requests
   activate, minimize, close).
2. `protocol/seat.xml`: `seat` (capabilities, get_pointer,
   get_keyboard, name), `pointer` (enter, leave, motion, button, axis,
   frame events; set_cursor with a surface and hotspot), `keyboard`
   (keymap event with a descriptor and size, enter with pressed keys,
   leave, key, modifiers, repeat_info). Serials on every input event
   are required by move, resize, popup grabs, set_cursor and data
   device operations.
3. Keymap: `/usr/share/keymaps/us.mkm`, a small binary table mapping
   key codes to symbols and characters for the plain, Shift, Ctrl and
   Alt levels, generated by `tools/genkeymap.py`; the compositor sends
   it once per keyboard as a memfd; libgui gains `gui/keymap.h` with
   loading and translation, and key repeat driven by `repeat_info`
   through an application timer.
4. `protocol/data.xml`: `data_device_manager`, `data_source` (offer
   mime, send with a descriptor, cancelled, dnd_finished),
   `data_offer` (offer events, receive with a descriptor, accept),
   `data_device` (set_selection, start_drag with a source, origin and
   icon surface; data_offer, enter, leave, motion, drop, selection
   events). Contents flow through pipes created by the receiver and
   passed to the source owner.
5. Compositor: `shell.c` (roles, configure and ack cycle, cascade
   placement, minimum and maximum sizes, maximize, minimize, close,
   popups with grabs and dismissal, layer surfaces with exclusive
   zones), `decor.c` (server side title bars, boxes, resize handles,
   move and resize drags, clamping; suppressed when a client chose
   client side decorations), `seat.c` (focus, enter and leave, pointer
   and keyboard delivery, serials, cursor surfaces with a default
   cursor image), `data.c` (selection and drag state, offers, transfer
   plumbing), keyboard shortcuts (Alt+Tab, Alt+F4, Alt+drag).
6. Panel `user/panel/`: a layer client anchored to the bottom with an
   exclusive zone, the launcher menu (`/etc/launcher`), the toplevel
   list from `toplevel_manager` with activate and minimize, the clock;
   it launches programs and reaps them. `startgui` starts the
   compositor, the panel and the requested client.
7. Document `docs/design/shell.md` (roles, configure protocol,
   decorations, seat, data device, panel).

Tests: boot tests `comp_shell` (toplevel configure and ack on a
resize started by the compositor's decoration drag, maximize, minimize
through the panel's manager protocol, popup dismissal on an outside
click), `comp_seat` (pointer enter, motion, button, axis with serials;
keymap sent and translated by the test client; modifiers; repeat
info), `comp_data` (clipboard between two test clients through a
passed pipe; drag and drop with enter, motion, drop and a received
payload), `comp_panel` (the panel lists a toplevel and launches
`clock`); host unit tests for the positioner constraint logic and the
keymap translation.

### M26. libgui port and application migration (completed 2026-08-30)

1. `libgui/src/client.c` rewritten on libwire: `gui_window` becomes a
   surface with a toplevel or popup role, an shm pool with two buffers
   (swap on commit, wait for release before reusing), damage
   accumulated by `gui_damage` and committed once per `app_step` when
   the previous frame callback has fired; server events become the
   framework's `struct gui_event` (renamed from `struct wmsg`; the
   `window_message` entry point keeps its shape); keyboard events
   translated with `gui/keymap.h`, key repeat from the application
   timer; `gui_set_title`, `gui_resize`, `gui_set_min_size` mapped to
   the toplevel requests; window resizes follow the configure and
   ack cycle (`WM_RESIZED` becomes the configure event, the framework
   relayouts and acknowledges on its next commit).
2. Popups: menus, combo boxes and tooltips become popup surfaces (M25
   `popup` with positioners) instead of floating widgets, so they can
   extend beyond their window; `window_popup_open` keeps its signature.
3. Clipboard and drag and drop: `gui_clipboard_set` and `get` over the
   data device; framework signals `drag_begin` (a widget starts a drag
   with text or file paths and an icon) and `drop` (`struct sig_drop`
   with mime type and contents); the editor, text field and file table
   support text and path drops.
4. `libgui` host tests: `fake_client.c` reimplemented over the new
   internal event structure; the existing framework tests keep passing.
5. Applications: `term`, `files`, `view`, `gedit`, `clock`, `paint`,
   `pong`, `mandel`, `widgettest`, `guitest`, `apptest`, `fonttest`
   unchanged except where they used `wmsg` fields directly; `wsrv`,
   `gui/proto.h`, the message queue transport and `wsrv.clip` removed;
   the `gui_*` boot tests updated to the compositor's log lines and
   the new geometry; `docs/design/gui.md` rewritten around the new
   stack, `docs/INTRODUCTION.md` updated.

Tests: all existing GUI cases under the new stack plus `gui_popup`
(a menu extends beyond its window and is dismissed by an outside
click on another window), `gui_dnd` (text dragged from `gedit` into a
second `gedit`), and `gui_frames` (a client that damages every frame
receives exactly one `done` per compositor frame, checked by counts).

## 4. Testing Strategy

- `tests/run_qemu_test.sh <case>` boots the image with `-display none -serial file:<out> -device isa-debug-exit,iobase=0xf4,iosize=0x4` and a timeout. The kernel writes `TEST PASS` or `TEST FAIL <reason>` to serial and exits through port `0xf4`.
- Kernel self tests are compiled in when `CONFIG_TESTS=1` and selected by a Limine command line argument such as `test=pmm`.
- User space tests in `user/tests/` run under a `runtests` program once M10 is reached and report the same markers.
- `make test` runs all cases and prints a summary.
- The shutdown path is itself tested: a case boots to user space, runs `shutdown`, and asserts that the block cache was flushed (the mfs clean flag is set on the resulting image) and that QEMU exited through the ACPI power off rather than the timeout.

## 5. Conventions

- Kernel code is C17, freestanding, no dynamic allocation before M5, no floating point.
- Headers in `kernel/include/` use the `#pragma once` guard. Every subsystem has one header named after its directory.
- Naming: `subsystem_verb_object`, for example `pmm_alloc_page`, `vfs_open`, `sched_yield`. Types are `struct name`, no typedef for structs. Fixed width integers from `<stdint.h>`.
- Errors are negative `errno` values returned as `int` or `long`. Pointers returning errors use `ERR_PTR` and `IS_ERR` helpers.
- Locks: every shared structure documents which lock protects it in a comment above the struct definition, and takes that lock from the first commit in which it exists. Disabling interrupts is never treated as sufficient mutual exclusion on its own.
- Per CPU state is only accessed through `cpu_current()`. No global variables hold per CPU data.
- Lock ordering is recorded in `docs/design/locking.md` before a new lock is introduced.
- Every milestone adds a boot test and a short document in `docs/design/`.

## 6. Tooling Requirements

Already installed: `x86_64-elf-gcc`, `nasm`, `qemu-system-x86_64`.
To install: `brew install x86_64-elf-gdb xorriso` (xorriso for ISO creation, needed only until M13 makes the disk image primary).

## Debugging tools (completed 2026-08-30)

Graphical tools `sysmon`, `logview` (over `/dev/klog`), `hexview`, `evtest` and `compsettings` (over the `debug` and `settings` protocol interfaces). Documented in `docs/design/tools.md`, tested by `tests/cases/gui_tools`.

## Desktop and settings (completed 2026-08-30)

Desktop layer client with wallpaper, icons of `/home/desktop`, context menus, MIME tables (`gui/mime.h`), the user settings application `settings` and layer surface windows in libgui. Documented in `docs/design/desktop.md`, tested by `tests/cases/gui_desktop` and the libgui host test.
