# MiniOS

MiniOS is a monolithic x86_64 kernel written in C with a custom user space. It boots through the Limine bootloader and runs under QEMU. The kernel implements physical and virtual memory management, processes and threads on several processors, a virtual filesystem with two disk filesystems, device drivers, an IPv4 network stack, and a POSIX subset of system calls. User space consists of a C library, a dynamic loader, an init that supervises services, a display server with a client toolkit, an audio server, a shell, command line utilities, a package installer, graphical applications, and the Lua 5.5 interpreter.

The release is `0.1.0`, recorded in `VERSION` under semantic versioning. `tools/version.sh` numbers every kernel link and records the commit hash and date. `uname`, the boot log and the System page of Settings print the release, the build number and the commit.

## Design decisions

| Area | Decision |
|---|---|
| Processor | x86_64, under QEMU with the Q35 machine. TCG, KVM and HVF are supported. |
| Boot | Limine boot protocol. The kernel is linked at `0xffffffff80000000`. The image boots under BIOS and UEFI. |
| Language | C17 freestanding, compiled with `x86_64-elf-gcc`. Assembly uses GNU as syntax. The kernel is compiled without floating point instructions. |
| Kernel | Monolithic. The scheduler runs on return to user mode. Kernel code runs until it returns, blocks or yields. |
| Per CPU state | One `struct cpu` per processor, reached through the GS base. The current thread is a field of that structure. |
| Locking | Every shared structure names the lock that protects it. `docs/design/locking.md` records the lock order. |
| Scope | One user. Permission bits are stored and ignored. Networking is IPv4 only, without forwarding or TLS. |
| Linking | Programs are ELF64 executables linked against shared libraries in `/lib`, loaded by `/lib/ld.so` with thread local storage, `dlopen` and lazy binding. `init` and the loader are static. |
| Storage | The root image is rebuilt by the build. The home directory and the user's configuration are on a data volume, `data.img`, that the build creates once and mounts at boot through `/etc/fstab`. |

## Memory management

The buddy allocator in `kernel/mm/pmm.c` allocates physical frames in orders 0 to 12 (4 KiB to 16 MiB). Each CPU has a cache of 32 order zero frames that is used before the free lists. The slab allocator in `kernel/mm/slab.c` allocates kernel heap objects in size classes from 16 bytes to 8 KiB, a magazine per CPU and cache, and optional red zones and poisoning.

Every process has an address space of regions. Pages are allocated on first access. `fork` shares pages copy on write. `mmap` creates anonymous and file backed regions, private or shared. `mprotect` changes their protection. `madvise` changes their flags, and `madvise(MADV_FREE)` marks pages that the swap daemon may discard without writing them. Anonymous private regions may be backed by 2 MiB pages, on request through `MAP_HUGETLB` or after `madvise(MADV_HUGEPAGE)`. The swap daemon writes anonymous pages to a virtio-blk swap device when the free frame count falls below a watermark, selecting pages with a clock algorithm. Resident page counts are kept in per CPU counters and reported through `/dev/meminfo`.

## Processes, threads and scheduling

A process consists of an address space, a file descriptor table, signal state, resource limits and a list of threads. Threads are scheduled by a multilevel feedback queue with eight levels. Each CPU has a run queue with a lock of that CPU and an inbox for wakeups sent by other CPUs. The scheduler locks are these per CPU run queue locks.

User threads have thread local storage through the FS base and block on futexes. The C library implements the pthread interface on these primitives and is safe to call from several threads.

Signals are implemented as in POSIX, with process groups, a controlling terminal and job control. `init` shuts the system down in order when it receives the shutdown signal. Each process has resource limits (`RLIMIT_AS`, `RLIMIT_DATA`, `RLIMIT_STACK`, `RLIMIT_NOFILE`, `RLIMIT_NPROC`, `RLIMIT_FSIZE`, `RLIMIT_CPU`) and user and system time accounting.

The kernel reads the CMOS clock at boot. `clock_gettime` returns `CLOCK_MONOTONIC` from the TSC based timer and `CLOCK_REALTIME` from that plus the boot offset.

## Symmetric multiprocessing

Application processors are started through the Limine multiprocessor protocol. Each processor calibrates its local APIC timer. Every unmap calls `tlb_flush_range`, which sends the shootdown interprocessor interrupts.

The common system call, byte stream and allocation paths use per CPU state and atomic operations in place of global locks. Lookups of file descriptors, signal state and regions are done under RCU and then increment a reference count. Statistics use per CPU counters. Wakeups and RCU callbacks are queued in multi-producer single-consumer queues. Byte streams use single-producer single-consumer rings. `docs/design/lockfree.md` states the ordering rules and the correctness argument for each path.

`/dev/lockstat` reports acquisition counts, contention and hold times per lock. `/dev/profile` samples the instruction pointer and the call chain of a process or of every process on a timer. A sample taken inside a spinlock section is attributed to the code that acquired the lock.

## Filesystems and storage

The VFS in `kernel/fs/vfs.c` resolves paths across mount points. Each process has a file descriptor table with `O_CLOEXEC` and `O_NONBLOCK` flags. `poll` accepts every descriptor type.

mfs is the native filesystem. It uses 4 KiB blocks. An inode has twelve direct pointers, one indirect pointer and one double indirect pointer. A zero pointer reads as a block of zeros. A write ahead journal is replayed at mount. The host tools `mkfs` and `fsck` build and check images. The FAT driver mounts FAT12, FAT16 and FAT32 volumes with long file names for reading and writing. The host tool `mkfat` builds images. initrdfs is a read only archive that is mounted before the root filesystem is mounted.

devfs contains the device nodes: `/dev/console`, `/dev/null`, `/dev/zero`, `/dev/urandom`, `/dev/fb0`, `/dev/vda`, `/dev/pcm0`, `/dev/input/eventN`, `/dev/ptmx` and `/dev/ptsN`, `/dev/klog`, `/dev/proc`, `/dev/maps`, `/dev/mounts`, `/dev/meminfo`, `/dev/net`, `/dev/ksyms`, `/dev/profile` and `/dev/lockstat`. The boot log lists the registered nodes in one line.

The block layer has a write back cache of 256 buffers of 4 KiB. The virtio-blk driver uses the modern PCI interface.

## Interprocess communication

The kernel implements pipes, Unix domain stream sockets with `socketpair` and descriptor passing through `SCM_RIGHTS`, anonymous shared memory through `memfd_create`, named shared memory through `shm_open`, message queues through `mq_open`, `eventfd`, `timerfd`, futexes and pseudo terminals. Abstract Unix socket names are supported; init's control socket uses one.

## Networking

`kernel/net/` implements IPv4 for one virtio-net interface and the loopback interface. Packets are handled by a worker thread and live in packet buffers that have exactly one owner at a time. The stack has Ethernet, ARP, IPv4 with fragment reassembly and path MTU discovery, ICMP, UDP and TCP with congestion control and retransmission timers. Sockets use the Internet ABI of POSIX (`AF_INET`, `SOCK_STREAM`, `SOCK_DGRAM`) behind the same socket layer as Unix domain sockets. `/dev/urandom` is seeded from virtio-rng when present. `dhcpc` configures the interface and the resolver, and the tools `net`, `ping`, `nc`, `http` and `xfer` use the stack. TCP has no window scaling, selective acknowledgements or timestamps, and the resolver has no cache. `docs/design/network.md` describes the stack and the validation records beside it list the evidence per milestone.

## Device drivers

| Driver | Function |
|---|---|
| `serial.c` | COM1. The console output is copied to it, and the boot tests read their results from it. |
| `fbcon.c`, `font.c` | Framebuffer console with an 8x16 bitmap font and SGR attributes. |
| `kernel/input/` | Input core. Events in the Linux format are written to `/dev/input/eventN` and to the console terminal. |
| `ps2kbd.c`, `ps2mouse.c` | PS/2 keyboard and mouse, reporting through the input core. |
| `virtio/virtio_input.c` | virtio-input keyboard and tablet. |
| `timer.c`, `apic.c`, `pit.c` | TSC time calibrated against the PIT at kernel entry, local APIC timer tick, I/O APIC routing. |
| `pci.c`, `virtio/virtio.c` | PCI enumeration and the virtio transport. |
| `virtio/virtio_blk.c` | Block storage. |
| `virtio/virtio_net.c` | Ethernet interface for the network stack. |
| `virtio/virtio_rng.c` | Entropy for `/dev/urandom`. |
| `virtio/virtio_gpu.c`, `fbdev.c` | Display through `virtio-vga` with mode setting at run time. |
| `virtio/virtio_snd.c`, `audio/pcm.c` | Playback and capture through `/dev/pcm0`. |
| `pty.c`, `tty.c` | Pseudo terminals and the line discipline. |
| `rtc.c` | CMOS real time clock. |
| `debugexit.c` | Exit from QEMU with a status code for the boot tests. |

## Display server and toolkit

The display server X12 (`user/compositor/`, installed as `/bin/x12`) opens the framebuffer and the input devices. Clients connect over a Unix domain socket. The protocol is defined in `protocol/*.xml`. `tools/wscan` generates the marshalling code. `libwire` implements the client and the server connection. The protocol is modelled on Wayland: surfaces with shared memory buffer pools, damage, a frame clock with frame callbacks, and roles for toplevels, popups with positioners and layer surfaces. The seat object transmits the keymap and the repeat settings. A data device implements the clipboard and drag and drop. The server composites with occlusion culling and alpha blending and logs to `/var/log/x12.log`.

The toolkit draws the window decorations: a header bar with the title and the close, maximize and minimize buttons, resize zones around the frame, and a shadow. When the server requests server side decorations, the toolkit draws the window contents without a frame.

`libgui` is the application framework. It has a retained widget tree with signals, box and grid layout, a theme read from `/etc/desktop.conf`, partial redraws, PNG and SVG images, and integer scaling for high density outputs. The widgets are label, button, check box, radio button, separator, canvas, text field, list view, scroll bar, scroll area, combo box, spinner, slider, progress bar, tabs, split pane, tool bar, status bar, menu bar, tree view, table with a model, and a text editor with undo, word wrap and syntax highlighting. `libfont` parses TrueType and CFF OpenType fonts and rasterizes antialiased glyphs with kerning.

The session consists of the panel (launcher menu, task list, volume mixer, clock), the desktop client (wallpaper, launcher files, context menus) and the Settings application (Appearance, Display, Keyboard, Sound, Date and time, File types, Launcher, System). `startgui` starts the session from the console shell.

## Audio

`/dev/pcm0` is the raw PCM device of the virtio-snd driver. Its period queue is interrupt driven and pollable. `audiod` opens the device, mixes the playback streams of its clients with a volume per stream, and makes the mix and the capture input available to capture streams. Samples are exchanged in shared memory pools. Control messages use the `audio.xml` protocol generated by `wscan`. `libaudio` implements the client side with a blocking interface and an event loop interface.

## User space

The C library in `libc/` has the headers `stdio.h`, `stdlib.h`, `string.h`, `math.h`, `time.h`, `pthread.h`, `signal.h`, `termios.h`, `dirent.h`, `fnmatch.h`, `glob.h`, `regex.h`, `wchar.h`, `locale.h`, `setjmp.h`, `fenv.h` and the `sys/` headers for sockets, memory mapping, waiting, file status, resource limits, event descriptors and audio. The math library has functions for `float`, `double` and x87 `long double`, the floating environment, hexadecimal and decimal conversion, `printf` floating formatting, and an SSE2 vector interface. The kernel saves the FXSAVE area of every thread.

`init` reads `/etc/init.conf`, runs its tasks in order and supervises its services with restart limits; the audio server and the DHCP client are services of the shipped configuration. `initctl` lists, starts, stops, restarts and reloads entries and requests the shutdown.

`/bin/sh` parses complete command trees (`if`, `for`, `while`, `until`, `case`, functions, subshells, brace groups), expands parameters, command substitutions, arithmetic and pathnames, applies redirections and here documents, and controls jobs. Interactive input is read by `libedit`, which implements line editing, history and completion.

The 86 programs in `user/coreutils/` are the file, text, process and system utilities (`ls`, `grep`, `find`, `xargs`, `sort`, `diff`, `gzip`, `less`, `man`, `ps`, `prof`, `prlimit`, `mount`, `sync`, `shutdown`) and the terminal games (`2048`, `snake`, `life`, `maze`, `matrix`, `sl`). `/bin/sed` is the sed of FreeBSD, `/bin/awk` the One True AWK of Brian Kernighan, `/bin/make` the public domain POSIX make pdpmake, `/bin/tar` the tar of sbase and `/bin/tcc` the Tiny C Compiler, all five compiled unmodified from `third_party/`. tcc compiles and links programs on minios against the shared libraries. `ar` is written for minios and produces the archive format of the GNU binutils. Manual pages are installed under `/usr/share/man`.

Lua 5.5.1 is compiled unmodified from `third_party/lua/src/` into `/bin/lua` and `/bin/luac`. `user/lua/` adds the modules `fs` (directory listing, file status, whole file reads and writes), `sys` (process start, MIME handlers, signals, system information) and `gui`, which binds the `libgui` framework. `user/share/apps/clock.lua` and `pong.lua` are the clock and pong applications written in Lua, and `code.lua` is a source editor for C, Lua and shell scripts with a run panel. Launcher files on the desktop start them. `require "thread"` starts native worker threads with their own Lua states, and `require "audio"` binds `libaudio`; the Lua Synthesizer is an eight voice instrument written in Lua on these two modules.

`pkg` installs, verifies and removes `.mpk` packages under `/home/.local`, on the data volume, and builds them. The desktop applications of `user/packages/` are built as packages into `/usr/share/packages`, and the loader searches `/home/.local/lib` after `/lib`.

`edit` is a console text editor, `gedit` a graphical one. `mint` is a small scripting language written before user space had floating point and `setjmp`. It is retained as an example interpreter.

The graphical applications are the terminal emulator (scrollback, alternate screen, selection), the file manager, the image viewer, the calculator with RPN and algebraic modes, paint, pong, a Mandelbrot renderer, a Unicode viewer, a clock, an audio player, a subtractive synthesizer, a sequencer, the profiler with a flame graph over `/dev/profile`, and the debugging tools `sysmon` (process table), `logview`, `hexview`, `evtest` and `x12settings`.

## Repository layout

```
minios/
  Makefile              all, image, run, gdb, test, test-kvm, check, check-sh, clean
  toolchain.mk          compiler flags and paths
  VERSION               release under semantic versioning
  data.img              persistent data volume, created by the build, ignored by git
  limine.conf           bootloader configuration
  PLAN.md               design summary and milestone record
  NETWORK_PLAN.md       plan and progress record of the network stack
  TERMINAL_PLAN.md      plan of the terminal userland
  kernel/
    arch/x86_64/        boot, GDT, IDT, paging, APIC, PIT, FPU, syscall entry, context switch, SMP, power
    mm/                 pmm, vmm, slab, mmap, vma, filemap, madvise, huge, swap, tlb
    sched/              thread, proc, mlfq, wait, elf, user
    sync/               spinlock, mutex, semaphore, condvar, rcu, lockstat
    ipc/                pipe, signal, shm, mqueue, socket, unix_socket, poll, eventfd, timerfd, futex
    net/                packet buffers, interfaces, worker, ethernet, arp, ipv4, icmp, udp, tcp/, inet_socket
    fs/                 vfs, file, devfs, initrdfs, mfs/, fat/
    block/              blockdev, bcache
    input/              core, keyboard
    audio/              pcm
    drivers/            serial, console, fbcon, fbdev, font, ps2kbd, ps2mouse, pci, pty, tty, rtc, timer, virtio/
    syscall/            table, sys_proc, sys_fs, sys_mm, sys_signal, sys_misc, sys_ipc, sys_rlimit
    debug/              panic, backtrace, symbols, profile, unwind
    lib/                printf, string, klog, kassert, crc32, cmdline, chacha, random
    tests/              kernel self tests, selected with test= on the command line
  libc/                 C library
  libfont/              font parser and rasterizer
  libwire/              protocol library, client and server side
  libaudio/             audiod client library
  libgui/               application framework and widgets
  libedit/              line editor
  protocol/             protocol definitions in XML
  user/
    init/  ld/  sh/  coreutils/  edit/  mint/  term/  compositor/  panel/  desktop/
    settings/  files/  calc/  audiod/  apps/  lua/  pkg/  packages/  tests/  etc/  share/  home/
  third_party/          Limine, Lua 5.5.1, FreeBSD sed, the One True AWK, pdpmake, the sbase tar, tinycc (a submodule), DejaVu, Noto Sans, Latin Modern, Unifont, Font Awesome
  tools/
    mkfs/  fsck/        mfs image tools
    mkfat/              FAT image tool
    gensyms/            kernel symbol table generator
    genfont/  genicons/  genkeymap/  wscan/
    netpeer/            host peer of the network tests
    mkpkg.sh            package builder used by make packages
    fold-develop.sh     daily merge of the bleeding-edge branches into develop
    run.sh              QEMU command line for make run and make gdb
    version.sh          build number and version stamp
  tests/
    run_qemu_test.sh    boots one case headless and checks the serial output
    run_all.sh          runs the selected cases
    cases/              one directory per boot test
    net/                network peer harness checks and fuzzers
  docs/
    design/             one document per subsystem
    postmortems/        analyses of defects
```

## Building and running

```sh
make                             # kernel, libraries and user programs
make image                       # ISO, root image and swap image in build/
make run                         # boot QEMU with serial on stdio
make gdb                         # boot QEMU halted with the gdbstub on port 1234
make test CASES="gui gui_wm"     # run the named boot tests
make test-kvm                    # run the processor dependent cases under KVM (Linux)
make check                       # host unit tests of libfont, libwire, libgui and the Lua modules
make check-sh                    # host unit test of the shell parser
make check-lua                   # host tests of the Lua modules
make check-net                   # self test of the network peer harness
make check-net-fuzz              # host fuzzing of the wire parsers and socket validators
make packages                    # build the application packages
tools/run.sh --help              # QEMU options. qemu.conf holds local defaults
```

`make run` reads `QEMU_*` variables and `RUNFLAGS`, for example `make QEMU_AUDIO=none run` or `make RUNFLAGS="--audio wav --smp 2" run`. `make image VIDEO=2560x1600@2` selects a framebuffer mode with doubled pixels for a high density display. The Display page of Settings changes the mode at run time.

A change is checked with the cases of the modules it modifies. Running all cases takes too long for that.

## Tests

A boot test is a directory under `tests/cases/` with the kernel command line, the regular expressions that the serial output must and must not match, and optional resources: a swap image, further disks, FAT images, an audio backend, a display or input device, and a script that runs after QEMU exits. The kernel prints `TEST PASS` or `TEST FAIL <reason>` on the serial line and exits through `isa-debug-exit`. There are 174 cases. The network cases run against a peer on the host. Host unit tests cover the font engine, the protocol library, the toolkit, the Lua modules and the shell parser, and host fuzzers cover the network wire parsers.

## Code size

The counts are code lines reported by cloc 2.10, without blank and comment lines. The `generated/` directory of libwire is excluded.

| Component | C | Headers | Other | Total |
|---|---|---|---|---|
| kernel | 32,260 | 4,201 | 307 assembly, linker script and make | 36,768 |
| libc | 10,739 | 1,874 | 120 assembly and make | 12,733 |
| libgui, libfont, libwire, libaudio, libedit | 13,383 | 1,056 | 144 make and SVG | 14,583 |
| user programs, the loader and tests | 40,851 | 1,448 | 2,173 Lua, 853 shell, 397 make, 205 Python, 90 assembly, 2 text | 46,019 |
| tools | 2,025 | | 701 Python, 540 shell | 3,266 |
| protocol | | | 437 XML | 437 |
| total | 99,258 | 8,579 | 5,969 | 113,806 |

The Lua sources in `third_party/lua/src/` add 21,410 code lines, the sed and awk sources in `third_party/sed/src/` and `third_party/awk/src/` 9,045, the make sources in `third_party/make/src/` 3,393 and the compiled part of the sbase tar 1,524, all unchanged from their origin. The scripts under `tests/` add 769 lines (524 shell, 126 C, 119 Python), not counting their text fixtures.

## State

The system boots on four processors to a shell on the framebuffer console, mounts the mfs root filesystem from a virtio-blk device and the data volume at `/home`, starts the audio server and the DHCP client as services of init, and starts the graphical session with `startgui`. Development continues by feature on branches from `bleeding-edge`, merged through `develop` to `main`.
