# libc and the first user programs (M10)

## Build

`libc/Makefile` produces `build/libc/libc.a` and `build/libc/crt0.o`.
User programs are compiled with `UCFLAGS` from `toolchain.mk` (static, no
PIC, SSE disabled because the kernel does not save FPU state yet) and
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
`tests/cases/shell` (`test=shell`) types a session into the keyboard line
buffer through `ps2kbd_feed_scancode` and then starts `/bin/sh`, checking
the builtin output, PATH lookup with arguments, the error messages and the
exit status of `exit 3`. The interactive path was also verified manually
by booting the default image and typing through QEMU's monitor.
