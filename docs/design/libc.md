# libc and the first user programs (M10)

## Build

`lib/libc/Makefile` produces `build/libc/libc.a`, `build/lib/libc.so` and
`build/libc/crt0.o`. User code is compiled with `UCFLAGS` from
`toolchain.mk` (PIC, and on x86_64 SSE2 enabled with saved FPU state and
AVX disabled, `UARCHFLAGS`). The code of the target architecture is in
`lib/libc/arch/$(ARCH)/`: on x86_64 `syscall.S`, `crt0.S`, `crti.S`, `crtn.S`,
`setjmp.S`, `fenv.c`, `math_long.c` (the x87 long double functions),
`math_x87.c` (the SSE2 square roots, the x87 partial remainder and arc
tangent) and the internal header `libc_arch.h` (the thread pointer, the
spin wait hint and the stack of a new thread). The public headers
`setjmp.h`, `fenv.h` and `minios/simd.h` take their architecture part from
`include/bits/<arch>/`. On aarch64 `lib/libc/arch/aarch64/` contains the
same assembly, an `fenv.c` over FPCR and FPSR, and `math_long.c`: the
binary128 `long double` functions with exact `truncl`, `frexpl`,
`ldexpl`, `fmodl` and `remainderl`, and exponential, logarithmic and
inverse tangent functions that reduce their argument and evaluate a
Taylor or arctangent series in binary128 arithmetic (A7), so that they
reach binary128 precision within a few units of the last place. libgcc
performs that arithmetic in software. `src/ldouble.h`
takes a `long double` apart for both formats (the classification, the
trigonometric argument reduction, `%La`, which prints the leading 64
significand bits).
Programs normally link against shared libraries at `0x400000` with
`/lib/ld.so` as their interpreter; init remains static. See `dynlink.md`
for the linker options and the static startup path.
`user/Makefile` builds one binary per directory
listed in `PROGS`, one per file under `coreutils/` and one per file under
`tests/`, all copied into `build/initrd_root/bin`, which `make initrd`
packs into `build/initrd.tar`.

## Runtime

`crt0.S` reads `argc`, `argv` and `envp` from the stack laid out by the
kernel, aligns the stack and calls `__libc_start`, which initializes the
main thread, sets `environ`, initializes stdio, invokes ELF constructors,
runs `main` and passes its result to `exit`. The loader supplies callbacks
for dynamic initialization and finalization; `AT_BASE` distinguishes those
from static startup, which uses the linker's array boundaries. `exit`
runs `atexit` handlers and ELF destructors, flushes the streams and calls
`_exit`. See `dynlink.md` for dependency and callback ordering.
`arch/x86_64/syscall.S` defines `__syscall6`; the wrappers turn a negative errno
result into `-1` with `errno` set. Numbers come from
`kernel/include/syscall_nums.h`.

## Headers

- `string.h`: the complete set of `mem*` and `str*` functions plus
  `strlcpy`, `strtok_r`, `strdup` and `strerror`.
- `stdio.h`: `printf`, `fprintf`, `dprintf`, `sprintf`, `snprintf` and
  the `v` forms on one formatter (`%d %i %u %x %X %o %p %s %c %%`, flags
  `- 0 + space #`, width and precision including `*`, `hh h l ll z t`),
  `FILE` with a 1 KiB buffer (stdout line buffered, stderr unbuffered),
  `puts`, `fputs`, `putchar`, `fwrite`, `getchar`, `fgetc`, `fgets`,
  `fread`, `fflush`, `setvbuf`, `perror`. `fopen`, `fdopen` and `fclose`
  fail with `ENOSYS` until the VFS (M11).
- `stdlib.h`: `malloc`, `calloc`, `realloc`, `free` (segregated free
  lists with boundary tags over `sbrk`, see below), `atoi`, `atol`, `strtol`, `strtoul`, `abs`,
  `labs`, `exit`, `_Exit`, `abort`, `atexit`, `getenv`, `setenv`, `rand`,
  `srand`, `qsort`.
- `unistd.h`: `write`, `read`, `fork`, `execve`, `execv`, `execvp` (PATH
  search, and a file that `execve` refuses with `ENOEXEC` runs as a script
  of `/bin/sh`, as POSIX requires), `_exit`, `getpid`, `getppid`, `sbrk`, `chdir`, `getcwd`,
  `sched_yield`; descriptor operations beyond the console fail with
  `ENOSYS` until M11.
- `sys/wait.h`: `wait`, `waitpid`, `wait4` and the `W*` macros matching
  the kernel's status encoding. `signal.h`: `kill` and signal numbers.
  `sys/thread.h`: `thread_create`, `thread_exit`, `thread_join`.
- `fcntl.h`, `dirent.h`, `sys/stat.h`: types and constants now, functions
  returning `ENOSYS` until M11. `errno.h`, `assert.h`, `ctype.h`,
  `sys/types.h`.
- `pthread.h` (M35): threads, mutexes, condition variables, keys, once,
  spin and read-write locks on the kernel's threads and futexes; `errno`
  is per thread. `time.h` and `sys/time.h` (M35): the real time and
  monotonic clocks, calendar conversion in UTC and in the local zone,
  `strftime`, `nanosleep`.
  `malloc` and every `FILE` are locked. See `docs/design/threads.md` and
  `docs/design/time.md`.
- `setjmp.h`: x86-64 System V `setjmp` and `longjmp`, including the rule
  that a zero value passed to `longjmp` is observed as one. `locale.h`
  supplies named locales, locale objects and `localeconv`,
  `langinfo.h` supplies `nl_langinfo` (`locale.md`), and `libintl.h`
  supplies message catalogues (`gettext.md`). `wchar.h` supplies wide strings and memory operations,
  restartable strict UTF-8 conversion, wide numeric conversion and
  wide-character stream input/output. `regex.h` supplies compiled POSIX
  basic and extended byte regular expressions, captures, BRE back
  references, counted repetition, named character classes, anchors and
  the `REG_ICASE`, `REG_NEWLINE`, `REG_NOSUB` and `REG_STARTEND` modes.
  Collation follows LC_COLLATE, and the character classes are those of
  Unicode in every locale.
- `time.h` additionally exposes the C `TIME_UTC`, `timespec_get` and
  `timespec_getres` interfaces and the POSIX zone state (`tzname`,
  `timezone`, `daylight`, `tzset`) of the local zone (`time.md`).
- Added for the Lua port (see `lua.md`): `ungetc`, `freopen`, `tmpfile`,
  `tmpnam`, `popen`, `pclose`, `getc_unlocked`, `flockfile`,
  `funlockfile`, `fseeko`, `ftello` in `stdio.h`; `system` and `mkstemp`
  in `stdlib.h`; `strcoll` in `string.h`; `isgraph` and `isblank` in
  `ctype.h`; `sig_atomic_t` in `signal.h`; `_setjmp` and `_longjmp` in
  `setjmp.h`. `tmpfile` and `tmpnam` use `/tmp`, an empty directory in
  the root image.

## Terminal userland helpers

- `fnmatch.h`: wildcard matching with `*`, `?`, brackets and character
  classes; pathname, leading-period, no-escape and case-fold flags.
- `glob.h`: directory walking and sorted path matches, with append,
  offset, mark, no-check and no-sort modes. `globfree` owns the result
  cleanup. Matching follows symbolic links to directories, since the
  walk opens every directory by name.
- `wchar.h`: `wcwidth`/`wcswidth` report the display width of a
  character from the Unicode tables described in `unicode.md`.
- `stdio.h`: `getline`/`getdelim` grow a caller-owned allocation, retain
  the delimiter and return the byte count or -1 at EOF/error.
- `term.h`: `term_use_color(fd)` requires a tty, nonempty nondumb TERM,
  and an unset NO_COLOR; `term_columns(fd)` uses the window size, then
  COLUMNS, then 80. `term_sgr` returns a small SGR string retained in the
  thread control block (a thread local variable would give the shared
  library a TLS segment, `dynlink.md`).

`libc_ext` additionally checks these helpers, including glob fixtures,
quoting flags, combining/wide characters and dynamically grown lines.

## Programs

- `init`: forks `sh` through `execvp`, reaps every child with `wait` and
  restarts the shell when it exits.
- `sh`: reads a line with `fgets`, splits on blanks, runs the builtins
  `cd`, `exit`, `pwd` and `help`, otherwise forks and `execvp`s the
  command and prints a note if the child was killed by a signal. `sh -c
  "command"` runs one command. Quoting, variables, pipes and background
  jobs arrive in M16.
- `hello`: prints its pid, parent pid and arguments and exits with 7.
- `coreutils/echo`: the first utility, needed by the shell test.

## Tests

`tests/cases/libc` runs `/bin/libctest`, which checks the formatter, the
string functions, `ctype`, number parsing including overflow, the
allocator, `qsort`, the environment, the streams and `atexit`.
`tests/cases/libc_ext` runs `/bin/libcexttest`, covering non-local jumps,
the C locale, UTF-8 split-sequence conversion and rejection, wide
strings and numbers, BRE/ERE matching and captures, leftmost-longest
alternation, back references, newline anchors, bounded execution, the
new `time.h` interfaces and the stream, process and temporary file
functions added for Lua.
`tests/cases/shell` (`test=shell`) types a session into the keyboard line
buffer through `ps2kbd_feed_scancode` and then starts `/bin/sh`, checking
the builtin output, PATH lookup with arguments, the error messages and the
exit status of `exit 3`. The interactive path was also verified manually
by booting the default image and typing through QEMU's monitor.

## Large allocations (M33)

`malloc` serves requests of 256 KiB or more with a private anonymous
`mmap` of their own and `free` unmaps them, while smaller blocks remain in
the heap over `sbrk`. Window surfaces and buffer pools are
re-created at every mode change; on the list they fragmented the heap
and each new size cost another 30 MiB that never came back.

## Heap allocator

The first fit list was replaced on 2026-09-06 after the Lua garbage
collection test showed allocation and freeing times growing with the
square of the number of live objects: every `malloc` walked the list
from its head and every `free` walked it to coalesce. `malloc.c` now
retains free blocks in bins by size, one bin per 16 bytes up to 512 bytes
and one per power of two above, with the bin links in the payload of
the free block. Every block has a 16 byte header with the payload size
and two flags; a free block also writes its size in its last 8 bytes,
and the header of the following block records that its predecessor is
free, so `free` coalesces with both neighbours in constant time. Each
`sbrk` region ends with a used sentinel header of size zero, so the
forward neighbour is always a valid header; a region that continues
the previous one at the break turns the old sentinel into the header of
the new block. The smallest payload is 32 bytes. `realloc` grows into a
free successor in place before it copies. Requests of 256 KiB or more
are mapped separately as before.

## Returning free memory (2026-10-05)

Until 2026-10-05 the heap never shrank. A process retained every page that its
small allocations had touched until it exited, and the system monitor
showed a resident size that rose at the start and never fell. Free memory
now goes back to the kernel in two ways. A free block at the top of the
heap of 128 KiB or more shrinks the heap with a negative `sbrk`, down to
64 KiB. A free block of 64 KiB or more inside the heap gives the whole
pages of its payload back with `madvise(MADV_DONTNEED)`, apart from its
links and its footer. The flag `F_RELEASED` marks such a block, so a later
merge with a neighbour gives back only the pages of the parts that were
still in use. The kernel maps a zero page at the next touch of a page that
was given back.

The case `mem_release` runs `/bin/memreleasetest`. It writes 20 MiB of
blocks of 1000 bytes and frees them below a block that remains in use, and
the resident size must fall by 16 MiB. After the top block is freed too,
it must be within 2 MiB of the size at the start. Memory allocated again
with `calloc` must read as zero.

## Additions for sed and awk

The ports of FreeBSD sed and the One True AWK (`sedawk.md`) added
`err.h`, `getopt`, `readv` and `writev`, the permission bit macros and
`lstat`, `fchmod`, `chmod`, `fchown` and `chown` (which returned 0
without effect until the ownership of U1, `users.md`), the `scanf` family in
`src/stdio/scan.c`, `asprintf`, `bsearch`, `random`, `mbtowc`, `wctomb`,
`mblen`, `getprogname` (set from `argv[0]` by `__libc_start`),
`wctype.h`, `strings.h`, `libgen.h`, `limits.h` with the POSIX limits,
`sys/cdefs.h`, `sys/param.h`, `sys/uio.h` and the BSD type names in
`sys/types.h`.

`scan.c` reads through a source with one character of push back, which
is what the grammar needs: `%d %i %u %o %x %X %p` collect the longest
valid prefix and convert it with `strtol` or `strtoul`, the floating
conversions collect a decimal or hexadecimal literal or `inf`/`nan` and
call `strtod`, `%s`, `%c` and `%[` copy characters, `%n` stores the
count consumed, `*` suppresses assignment, and `hh h l ll j z t L` set
the width of the target. A directive that fails on the first input
character before anything was assigned returns `EOF`.

`wctype.h` classifies every code point with tables generated from the
Unicode Character Database, and `towupper` and `towlower` apply its simple
case mappings (`unicode.md`).

## Additions for make

The port of pdpmake (`make.md`) added `utimensat` with `AT_FDCWD`,
`UTIME_NOW` and `UTIME_OMIT`, `strndup`, `stpcpy`, `realpath`, `access`
with its four mode bits, `confstr` for `_CS_PATH` and the header `ar.h`.
`struct stat` gained `st_mtim`, filled with nanoseconds on mfs and with
two second resolution on FAT; `st_mtime` is now a macro for
`st_mtim.tv_sec`, so programs that use the old name compile unchanged.

## Additions for ar and tar

The ar utility and the sbase tar (`artar.md`) added `openat` and
`fstatat`, `pwd.h` and `grp.h` describing the single user (replaced by
the readers of the account databases in U0 of the multiuser plan,
`users.md`), the uid and gid functions, `lchown`, `symlink` and `readlink` (refused until the
symbolic links of 2026-09-30, system calls since then, with `symlinkat`,
`readlinkat` and `lstat`), `mknod` and `mkfifo` (refused), `execl`,
`execlp` and `sys/sysmacros.h`.

## Additions for tcc

The port of the Tiny C Compiler (`tcc.md`) added `dlfcn.h` over the
libraries the loader mapped (`dlopen`, `dlsym`, `dlclose`, `dlerror`),
`strtoll`, `strtoull`, `strtold`, `inttypes.h` with `strtoimax`,
`strtoumax` and `imaxabs`, a `stdint.h` of the C library, a
`limits.h` that gives the integer limits itself when the compiler is not
gcc, and `crti.o` and `crtn.o` with empty `_init` and `_fini`.

## Additions for multiple users

The multiuser plan (`users.md`) added `shadow.h`, `minios/account.h` and
the readers of the account databases in `pwd.h` and `grp.h`, `crypt` with
SHA-256 crypt, `getlogin`, the identity calls from `getuid` to
`setgroups`, `umask`, `faccessat`, `fchmodat` and `fchownat`, and
`mount_options` for the option string of `mount`. `minios/conf.h` gained
`conf_home`, the home of the caller, and `conf_user_file` and
`conf_user_write_file` for any file of `$HOME/.config`. B4 of
`docs/plan/desktop-panel.md` added `minios/init.h` with `init_request`,
which sends a request to init on its control socket (`init.md`), and B3
the item `_NL_FIRST_WEEKDAY` of `langinfo.h` (`locale.md`). B6 added
`conf_lookup` to `minios/conf.h`, which reads the value of one key of a
file of `key=value` lines. `conf_export_locale`, the greeter and the key
map reload of X12 use it in place of their own readers.

The ports of su, doas and sudo in U5 added `syslog.h` (`openlog`,
`syslog`, `vsyslog`, `setlogmask` and `closelog`, which append to
`/var/log/messages` and understand `%m`, `LOG_PID`, `LOG_CONS` and
`LOG_PERROR`), `getpass`, which reads `/dev/tty` with echo off after
discarding the typed input, `ttyname` and `ttyname_r`, `sysconf`,
`gethostname`, `fchdir`, `alarm`, `setsid`, `getsid`, `killpg`,
`sigpending`, `clearenv`, `strsep`, `getpwnam_r`, `getpwuid_r`,
`getgrnam_r`, `getgrgid_r`, `getspent`, `setspent` and `endspent`. The
terminal functions gained `tcflush`, `tcdrain`, `tcsendbreak`, the `cf*`
speed functions, `cfmakeraw` and the `TCSA*` actions, and `openpty`
took the BSD signature with the slave descriptor, the name, the settings
and the window size. `strtok_r` takes `char **` as POSIX has it.
`getpriority` and `setpriority` report and accept priority 0, `utime`
sets the modification time, and `chroot` fails with `ENOSYS`. New
headers are `paths.h`, `utmp.h` (types only, minios retains no login
records), `poll.h`, `utime.h`, `alloca.h`, `net/if.h` and
`netinet/tcp.h`.

## Additions for the binutils

The binutils of 2026-10-05 (`binutils.md`) added `elf.h` with the types
and constants of the System V ABI and of the processor supplements for
x86_64 and aarch64, under the names that other systems use. The header
also contains the ELF32 types, although minios reads and writes ELF64
files only. `minios/elffile.h` (`src/elffile.c`) is a reader of ELF64
little-endian files in memory. `elffile_open` checks the file header and
the section and program header tables against the size of the file. The
other functions check each offset and size that they follow and return
NULL for a part outside the file. The reader returns the sections by
index, name or type, the strings of a string table, the symbols of a
symbol table, the dynamic entries, and the names of the constants as
the GNU binutils print them. `pkg` (`user/pkg/elf.c`), the binutils and
`libprof` use the reader. The dynamic loader `/lib/ld.so` cannot link
the libc, and it uses only the types of `elf.h`.

## Additions for file transfer (2026-10-05)

`minios/crc32.h` declares `crc32(crc, data, n)`, the CRC-32 of gzip, zlib
and PNG, which continues the checksum `crc` over `n` more bytes. The
kernel has the same function in `kernel/lib/crc32.c`. `gzip_crc32`, the
PNG encoder of libcodec, `xfer` and the host tools `fsck` and `mkgpt`
used private copies before; the host tools compile `src/crc32.c` with the
host libc. `errno.h` gained `EPROTO` (71) and `ESTALE` (116) with the
values of Linux, for the protocol of `filetransfer.md`.

## Copy and fill functions (2026-10-06)

`memcpy`, `memmove` and `memset` move 16 bytes per load and store
through the vector type `simd_u64x2` of `bits/simd_types.h`, which is an
SSE2 register on x86_64 and a NEON register on aarch64. A copy of 16
bytes or more loads its first and its last 16 bytes before any store and
stores them after the main loop. The main loop stores whole aligned
blocks of the destination with unaligned loads from the source, so the
relative alignment of the two regions does not matter. Before, `memcpy`
copied single bytes when the two addresses differed in their low three
bits, and `memmove` always copied single bytes. A copy of fewer than 16
bytes uses two overlapping moves of 8 or 4 bytes, or three single bytes.
Every load precedes every store in that case, so the same code serves
`memmove`. `memmove` copies downwards when the destination overlaps the
source from above.

GCC can replace a copy or fill loop with a call of `memcpy` or `memset`.
Inside these functions the call would recurse, so they carry the
attribute `optimize("no-tree-loop-distribute-patterns")`. `libctest`
checks every pair of source and destination offsets from 0 to 31 at 39
lengths up to 65537 bytes, overlapping `memmove` in both directions and
`memset` with the guard bytes on both sides, and prints the throughput.
On x86_64 under TCG the copy of 1 MiB rose from 6469 to 8563 MB/s
aligned and from 5020 to 6759 MB/s at an offset of 4 bytes, and the fill
from 10126 to 13157 MB/s. On aarch64 with HVF the fill rose from 51 to
98 GB/s.

## Integer vectors (2026-10-06)

`bits/simd_types.h` gained the integer vector types `simd_u16x8` and
`simd_u8x16`, and `minios/simd.h` the unaligned `simd_load_u32x4` and
`simd_store_u32x4` and the attribute `SIMD_WITHIN_PAGE(bytes)`. The pixel
module of libgui uses them (`graphics-performance.md`).
`SIMD_WITHIN_PAGE` aligns a function to a power of two above its size,
so that the function never crosses a page boundary. QEMU under TCG does
not chain the translated blocks of a loop across a page boundary, which
made a pixel loop six times slower when the linker happened to place it
across one. `memmove`, `memset` and the copy loop of `memcpy` carry the
attribute as well.

## The process table (2026-10-06)

`minios/proctab.h` reads the table of `/dev/proc`. `proc_table_read`
fills an array of `struct proc_entry` with the columns PID, PPID, PGID,
STATE, TIME, RSS, UID and NAME of every process. `proc_table_find`
returns the row of one pid, or `-ESRCH`. The functions replaced the
private parsers of `ps`, `sysmon`, `profiler`, `wireview`, `libprof`,
`credtest` and `memreleasetest` (X1 of `docs/plan/x12settings.md`).
`libctest` checks the row of its own process and the row of the
kernel.

## Microseconds since boot (2026-10-06)

`uptime_us()` in `unistd.h` returns the microseconds of `CLOCK_MONOTONIC`,
beside `uptime_ms()`. The frame statistics of X12 and libgui use it
(`graphics-performance.md`). It replaced the private helper of
`sleeplattest`.

## Additions for MP3 (2026-10-06)

`malloc.h` includes `stdlib.h`. Ported code includes the header for the
declarations of the allocator, as the MP3 encoder shine does
(`codecs.md`).

## Interrupted sleeps (2026-10-05)

The system call `sleep_ms` returns `-EINTR` when a signal, a stop or the
exit of the process ends the sleep, as the other blocking calls do.
`nanosleep` then returns -1 with `errno` set to `EINTR` and stores the
requested time minus the time slept in `remain`. `sleep` returns the
unslept seconds, rounded up, and `usleep` returns -1 with `EINTR`. An
ignored signal does not end a sleep. Before this change every wake of the
thread ended the sleep, and the functions reported a complete sleep
(`docs/postmortems/2026-10-05-sleep-wakeup.md`).
