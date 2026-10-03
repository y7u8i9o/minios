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

The compiler is built for the target of the system, `TCC_TARGET_X86_64` on
x86_64 and `TCC_TARGET_ARM64` on aarch64 (`TCC_TARGET` in `toolchain.mk`,
A9). The bound checker, the
backtrace support and the semaphore lock are disabled. `CONFIG_TCC_STATIC`
is not set: tcc resolves the symbols of a program run with `-run`
through `dlfcn.h`. The paths are
`CONFIG_TCCDIR /usr/lib/tcc` (the runtime library and the compiler's own
headers), `CONFIG_TCC_SYSINCLUDEPATHS {B}/include:/usr/include`,
`CONFIG_TCC_LIBPATHS {B}:/usr/lib`, `CONFIG_TCC_CRTPREFIX /usr/lib`
and `CONFIG_TCC_ELFINTERP /lib/ld.so`, the interpreter every program
names, through the link `/lib` to `/usr/lib`. The object depends on the Makefile, so
that a change of these flags rebuilds it.

On aarch64 a program that writes code must clean the data cache and
invalidate the instruction cache before it runs that code. `tcc -run` calls
`__clear_cache` from libgcc for this, which runs the cache maintenance
instructions and reads `CTR_EL0` at EL0. The kernel enables both on every
CPU (`SCTLR_EL1.UCI` and `UCT`, `cpu_init_el0_access`).

## Installed files

- `/usr/lib/tcc/libtcc1.a`: the runtime library, built from `lib/` with
  the cross compiler: `builtin.c`, `stdatomic.c`, `atomic.S`,
  `dsohandle.c`, `alloca.S` and `alloca-bt.S`, on x86_64 with
  `libtcc1.c` (integer and floating conversions) and `va_list.c`, and on
  aarch64 with `lib-arm64.c` (the binary128 `long double` arithmetic) and
  `armflush.c` (`__clear_cache`). `armflush.c` is compiled with
  `__arm64_clear_cache` defined as the GCC builtin `__builtin___clear_cache`. `tcov.c` is left out, because the
  coverage support needs the file locks of `fcntl`, and the kernel
  implements none. `runmain.o` is installed beside the library.
- `/usr/lib/tcc/include/`: tcc's own `stdarg.h`, `stddef.h`, `stdbool.h`,
  `float.h`, `stdatomic.h`, `stdalign.h`, `stdnoreturn.h`, `tgmath.h`,
  `varargs.h` and `tccdefs.h`.
- `/usr/include/`: the libc headers, `minios/*.h` and `syscall_nums.h`
  from the kernel, and the headers of the libraries: `gui/`, `font/`,
  `wire/`, `audio/`, `edit.h` and `lua/`. `stdint.h` and `limits.h` were
  written for both compilers, since tcc ships neither and the libc
  `limits.h` used to reach the compiler's copy with `#include_next`.
  `minios/abi.h` includes `stddef.h` itself now; every header compiles
  alone.
- `/usr/lib/crt1.o`, a copy of `crt0.o`, `/usr/lib/crti.o`, which defines
  empty `_init` and `_fini` functions, and `/usr/lib/crtn.o`, which is
  empty. tcc links them
  around every program. `__libc_start` of a static program tests the
  weak `_init` and `_fini` through the global offset table, and tcc
  leaves the table slot of an undefined weak function pointing at a
  stub; without the definitions a static program crashed at exit.
- `/usr/lib/libm.a`, an empty archive for `-lm`, since the math functions
  are in `libc.so`. `/usr/lib/libc.a`, `libcodec.a`, `libgui.a`, `libfont.a`,
  `libwire.a`, `libaudio.a` and `libedit.a`, the static archives of the
  cross build, for `tcc -static`. A static program that uses libgui must
  also link `-lcodec`. A static program has no dynamic loader and cannot
  load the codec modules, and its image functions therefore fail with
  `ENOTSUP` (`codecs.md`).
- `/usr/share/tcc/tests2/`: the selected programs of the tcc test suite
  with their expected outputs.

## Build settings

A program compiled on minios and a program of the cross build meet the
same interface. Both see the same headers, since `/usr/include` is a copy
of the headers the cross build uses; both link the same objects, since
the archives and shared objects in `/usr/lib` are the ones the cross build
produces, compiled with `-fPIC`, `-msse2 -mfpmath=sse` and
`-fno-builtin`; and both use the SysV calling convention, the same
structure layouts and the same 80 bit `long double`. tcc predefines
`__linux__` and `__unix__` for its ELF targets, and `__GNUC__` with a
version, which the minios headers do not test. `make check-headers`, part
of `make check`, compiles every installed header on its own with the
cross compiler in C17; the tcc test does the same with tcc on minios,
except for `minios/simd.h`, whose vector types are a gcc extension.

## ld and as

`ld` (`user/coreutils/ld.c`) and `as` (`user/coreutils/as.c`) run
`/bin/tcc` under the conventional names, for makefiles and scripts that
call them. `ld` passes `-nostdlib`; nothing is linked beyond what is
named. It translates the options tcc implements: `-o`, `-L`, `-l`,
`-shared`, `-static`, `-r`, `-rdynamic`, `-soname`, `-Bsymbolic`,
`-rpath` and `-Map`. Options that name what tcc does in any case,
`-e _start`, `-dynamic-linker /lib/ld.so`, `-Ttext-segment=0x400000`,
`-z`, `--hash-style`, `--as-needed` and the group markers, are accepted
and dropped; another entry point, interpreter or text address, and any
other option, is an error. `as` assembles one file with `tcc -c` and
writes `a.out` without `-o`.

## Linking

tcc links against `/lib/libc.so` and the other shared objects by reading
their dynamic symbol tables, records them as `DT_NEEDED`, and writes
executables at `0x400000` with `/lib/ld.so` as the interpreter, a SysV
and a GNU hash table and the relocation types the minios loader applies.
`tcc -shared` writes a shared object that the loader maps from `/lib`.

## libc additions

- `dlfcn.h`: `dlopen`, `dlsym`, `dlclose` and `dlerror`, since
  2026-09-15 forwarded to the loader (`dynlink.md`), which loads
  libraries at run time; `tcc -run` resolves the references of the
  compiled program through `dlsym` on the program's handle.
- `strtoll`, `strtoull`, `strtold` (with the precision of `double`),
  `strtoimax`, `strtoumax`, `imaxabs` and `inttypes.h`.
- `stdint.h` and the self contained `limits.h`.

## Tests

`tests/cases/tcc` runs `/etc/tests/tcc.sh`: a program compiled and run
with its exit status and output, `-run`, `-v`, `-E`, compilation to
objects and linking them, `tcc -shared` with a program linked against the
result and run through the loader, the libc headers with `malloc`,
`sqrt`, `INT_MAX` and `INT64_MAX`, and the rejection of an undefined
function and of a syntax error. It also links a program against the toolkit, the protocol library, the
audio client and the Lua core, dynamically and statically, compiles
every installed header alone, runs a statically linked program, links a
program and a shared object with `ld`, and assembles a function with
`as` and calls it from C. It
then compiles and runs 54 programs of
the upstream test suite from `/usr/share/tcc/tests2` and compares the
diagnostics and the output with the expected text, with trailing white
space ignored as the upstream `diff -b` does. The selection leaves out
the programs that need arguments, input files, bound checking, threads,
thread local storage or a specific assembler, and the array assignment
test that the upstream suite skips as well.
