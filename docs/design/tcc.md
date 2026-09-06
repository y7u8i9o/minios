# tcc

`/bin/tcc` is the Tiny C Compiler, release 0.9.28rc, compiled unmodified
from the git submodule `third_party/tinycc`. It compiles, assembles and
links C programs on minios into executables and shared objects for
minios, and runs a program from source with `-run`.

## Sources

`third_party/tinycc` is a submodule of the upstream repository
(`https://github.com/TinyCC/tinycc.git`); `git submodule update --init`
fetches it after a clone. `third_party/tinycc-NOTICE` records the licence
(LGPL 2.1). Nothing in the submodule is modified: `user/Makefile` compiles
`tcc.c`, which includes every other source when `ONE_SOURCE` is set, with
the configuration on the command line and an empty `config.h`. The
files are exempt from the minios line count and naming conventions.

## Configuration

The compiler is built for the x86_64 target only. The bound checker, the
backtrace support and the semaphore lock are disabled. `CONFIG_TCC_STATIC`
is not set: tcc resolves the symbols of a program run with `-run`
through `dlfcn.h`. The paths are
`CONFIG_TCCDIR /usr/lib/tcc` (the runtime library and the compiler's own
headers), `CONFIG_TCC_SYSINCLUDEPATHS {B}/include:/usr/include`,
`CONFIG_TCC_LIBPATHS {B}:/lib`, `CONFIG_TCC_CRTPREFIX /lib` and
`CONFIG_TCC_ELFINTERP /lib/ld.so`. The object depends on the Makefile, so
that a change of these flags rebuilds it.

## Installed files

- `/usr/lib/tcc/libtcc1.a`: the runtime library, built from `lib/` with
  the cross compiler: `libtcc1.c` (integer and floating conversions),
  `va_list.c`, `builtin.c`, `stdatomic.c`, `atomic.S`, `dsohandle.c`,
  `alloca.S` and `alloca-bt.S`. `tcov.c` is left out, because the
  coverage support needs the file locks of `fcntl`, and the kernel
  implements none. `runmain.o` is installed beside the library.
- `/usr/lib/tcc/include/`: tcc's own `stdarg.h`, `stddef.h`, `stdbool.h`,
  `float.h`, `stdatomic.h`, `stdalign.h`, `stdnoreturn.h`, `tgmath.h`,
  `varargs.h` and `tccdefs.h`.
- `/usr/include/`: the libc headers, `minios/*.h` and `syscall_nums.h`
  from the kernel. `stdint.h` and `limits.h` were written for both
  compilers, since tcc ships neither and the libc `limits.h` used to
  reach the compiler's copy with `#include_next`.
- `/lib/crt1.o` (a copy of `crt0.o`), `/lib/crti.o` and `/lib/crtn.o`
  (empty objects), the runtime objects tcc links around every program;
  `/lib/libm.a`, an empty archive for `-lm`, since the math functions are
  in `libc.so`.
- `/usr/share/tcc/tests2/`: the selected programs of the tcc test suite
  with their expected outputs.

## Linking

tcc links against `/lib/libc.so` and the other shared objects by reading
their dynamic symbol tables, records them as `DT_NEEDED`, and writes
executables at `0x400000` with `/lib/ld.so` as the interpreter, a SysV
and a GNU hash table and the relocation types the minios loader applies.
`tcc -shared` writes a shared object that the loader maps from `/lib`.

## libc additions

- `dlfcn.h`: `dlopen`, `dlsym`, `dlclose` and `dlerror` over the
  libraries the loader mapped at start, found through `/dev/maps`.
  `dlopen` returns a handle for a library that is mapped in the process
  and fails for any other; `dlsym` reads the dynamic symbol table of the
  library file once and adds the base address. `tcc -run` resolves the
  references of the compiled program through them.
- `strtoll`, `strtoull`, `strtold` (with the precision of `double`),
  `strtoimax`, `strtoumax`, `imaxabs` and `inttypes.h`.
- `stdint.h` and the self contained `limits.h`.

## Tests

`tests/cases/tcc` runs `/etc/tests/tcc.sh`: a program compiled and run
with its exit status and output, `-run`, `-v`, `-E`, compilation to
objects and linking them, `tcc -shared` with a program linked against the
result and run through the loader, the libc headers with `malloc`,
`sqrt`, `INT_MAX` and `INT64_MAX`, and the rejection of an undefined
function and of a syntax error. It then compiles and runs 54 programs of
the upstream test suite from `/usr/share/tcc/tests2` and compares the
diagnostics and the output with the expected text, with trailing white
space ignored as the upstream `diff -b` does. The selection leaves out
the programs that need arguments, input files, bound checking, threads,
thread local storage or a specific assembler, and the array assignment
test that the upstream suite skips as well.
