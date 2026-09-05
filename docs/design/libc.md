# libc and the first user programs (M10)

## Build

`libc/Makefile` produces `build/libc/libc.a` and `build/libc/crt0.o`.
User programs are compiled with `UCFLAGS` from `toolchain.mk` (static, no
PIC, SSE2 enabled with saved FPU state, AVX disabled) and
linked with `-nostdlib -static -Ttext-segment=0x400000`, `crt0.o`,
`libc.a` and `libgcc`. `user/Makefile` builds one binary per directory
listed in `PROGS`, one per file under `coreutils/` and one per file under
`tests/`, all copied into `build/initrd_root/bin`, which `make initrd`
packs into `build/initrd.tar`.

## Runtime

`crt0.S` reads `argc`, `argv` and `envp` from the stack laid out by the
kernel, aligns the stack and calls `__libc_start`, which sets `environ`,
initializes stdio, runs `main` and passes its result to `exit`. `exit` runs
`atexit` handlers in reverse order, flushes the streams and calls `_exit`.
`syscall.S` provides `__syscall6`; the wrappers turn a negative errno
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
- `stdlib.h`: `malloc`, `calloc`, `realloc`, `free` (first fit list over
  `sbrk` with coalescing), `atoi`, `atol`, `strtol`, `strtoul`, `abs`,
  `labs`, `exit`, `_Exit`, `abort`, `atexit`, `getenv`, `setenv`, `rand`,
  `srand`, `qsort`.
- `unistd.h`: `write`, `read`, `fork`, `execve`, `execv`, `execvp` (PATH
  search), `_exit`, `getpid`, `getppid`, `sbrk`, `chdir`, `getcwd`,
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
  monotonic clocks, calendar conversion in UTC, `strftime`, `nanosleep`.
  `malloc` and every `FILE` are locked. See `docs/design/threads.md` and
  `docs/design/time.md`.
- `setjmp.h`: x86-64 System V `setjmp` and `longjmp`, including the rule
  that a zero value passed to `longjmp` is observed as one. `locale.h`
  supplies the deterministic `C`/`POSIX` locale and its complete
  `lconv`. `wchar.h` supplies wide strings and memory operations,
  restartable strict UTF-8 conversion, wide numeric conversion and
  wide-character stream input/output. `regex.h` supplies compiled POSIX
  basic and extended byte regular expressions, captures, BRE back
  references, counted repetition, named character classes, anchors and
  the `REG_ICASE`, `REG_NEWLINE`, `REG_NOSUB` and `REG_STARTEND` modes.
  Collation and character classes follow the sole C locale.
- `time.h` additionally exposes the C `TIME_UTC`, `timespec_get` and
  `timespec_getres` interfaces and the POSIX UTC timezone state
  (`tzname`, `timezone`, `daylight`, `tzset`).

## Terminal userland helpers

- `fnmatch.h`: wildcard matching with `*`, `?`, brackets and character
  classes; pathname, leading-period, no-escape and case-fold flags.
- `glob.h`: directory walking and sorted path matches, with append,
  offset, mark, no-check and no-sort modes. `globfree` owns the result
  cleanup. The VFS has no symbolic links.
- `wchar.h`: `wcwidth`/`wcswidth` report zero-width combining and
  double-width ranges for cursor and column arithmetic.
- `stdio.h`: `getline`/`getdelim` grow a caller-owned allocation, retain
  the delimiter and return the byte count or -1 at EOF/error.
- `term.h`: `term_use_color(fd)` requires a tty, nonempty nondumb TERM,
  and an unset NO_COLOR; `term_columns(fd)` uses the window size, then
  COLUMNS, then 80. `term_sgr` returns a small thread-local SGR string.

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
alternation, back references, newline anchors, bounded execution and the
new `time.h` interfaces.
`tests/cases/shell` (`test=shell`) types a session into the keyboard line
buffer through `ps2kbd_feed_scancode` and then starts `/bin/sh`, checking
the builtin output, PATH lookup with arguments, the error messages and the
exit status of `exit 3`. The interactive path was also verified manually
by booting the default image and typing through QEMU's monitor.

## Large allocations (M33)

`malloc` serves requests of 256 KiB or more with a private anonymous
`mmap` of their own and `free` unmaps them, while smaller blocks stay on
the first fit list over `sbrk`. Window surfaces and buffer pools are
re-created at every mode change; on the list they fragmented the heap
and each new size cost another 30 MiB that never came back.
