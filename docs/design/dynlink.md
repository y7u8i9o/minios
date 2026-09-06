# Dynamic linking

Programs are linked against shared libraries since 2026-09-06. The C
library, the toolkit stack, the audio client library and the Lua core are
shared objects in `/lib`, loaded into each process by `/lib/ld.so`, the
dynamic loader of minios. `init` and the loader itself are static.

## Measurements

These measurements describe the initial conversion, before the additional
loader test programs and libraries below were installed.

Before the change the 149 programs of the root image took 62.6 MiB, of
which 13.9 MiB was code and data and the rest DWARF debugging information,
because the unstripped link output was installed. Every program contained
the C library (137 KiB of text), and the 25 graphical programs contained
the toolkit, the font engine and the protocol library (about 230 KiB).
After the change the programs take 2.2 MiB together and the seven shared
objects 870 KiB; `echo` is 3 KiB, the file manager 39 KiB, the shell 76 KiB.

| Library | Programs using it | Shared |
|---|---|---|
| libc | all | yes |
| libgui, libfont, libwire | 25 graphical programs | yes |
| libaudio | 10 | yes |
| liblua (the Lua core) | lua, luac | yes; luac links the core statically, since it calls internal functions that the shared object hides |
| libedit | sh | no, static |

## Build

`toolchain.mk` compiles all user code with `-fPIC`, so the same objects go
into the archives and the shared objects. Each library Makefile links
`$(BUILD)/lib/<name>.so` with `ld -shared -z now --hash-style=sysv
-soname <name>.so` and names the libraries it depends on, so that the
loader finds them through `DT_NEEDED`; the compiler driver of the bare
metal target does not pass `-shared` on, hence the direct use of `ld`.
`libgcc.a` is linked into every shared object. The libc rule fails when
the object carries a TLS segment.

Programs link with `ULDFLAGS`: `-Ttext-segment=0x400000`,
`-dynamic-linker /lib/ld.so`, `-z now`, `--hash-style=sysv` and
`--as-needed`, against `-lgui -laudio -lwire -lfont libedit.a -lc`.
`--as-needed` records only the libraries a program references. Programs
stay `ET_EXEC` at `0x400000`; the linker resolves their references to the
libraries with `R_X86_64_JUMP_SLOT` (functions), `R_X86_64_GLOB_DAT`
(data through the global offset table) and, for data referenced from
assembly, `R_X86_64_COPY`. `init` uses `ULDFLAGS_STATIC`.

`user/Makefile` installs every program and library with `objcopy
--strip-debug`, which removes the DWARF sections and keeps the symbol
table the profiler reads. The unstripped files stay in `build/user/` and
`build/lib/` for gdb.

## Kernel

`sched/elf.c` records the address of the program headers when a `PT_LOAD`
segment covers them and the path of `PT_INTERP` in `struct elf_info`.
`elf_load_interp` loads the loader, an `ET_DYN` file, with every segment
shifted by `USER_INTERP_BASE` (`0x7e0000000000`). `load_image` in
`sched/user.c` reads the loader file after the program and starts the
thread at the loader's entry. `user_stack_setup` appends the auxiliary
vector after the environment: `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`,
`AT_PAGESZ`, `AT_BASE` (the loader's base), `AT_ENTRY` (the program's
entry) and `AT_NULL`. A static program receives the same vector with
`AT_BASE` 0; libc uses it to distinguish static startup from the callback
handoff described below.

## The loader

`user/ld/ld.c` and `start.S` build `/lib/ld.so`, a freestanding position
independent object linked with `-Bsymbolic`, hidden visibility, `-fno-plt`
and its own entry `_dl_start`; it uses no libc and makes its system calls
through an inline `syscall`. `_dl_main` receives the initial stack pointer
and returns the program's entry point, which `start.S` enters with the
stack pointer the kernel provided.

The loader first applies its own relocations, which are all
`R_X86_64_RELATIVE`; the address of its dynamic section is taken through
a hidden symbol, so that no global offset table entry is read before it
is relocated. It then reads the program's `PT_DYNAMIC` from the headers
named by `AT_PHDR`, and loads the libraries named by `DT_NEEDED` breadth
first: the program's own libraries in order, then theirs. A library is
opened as `/lib/<soname>`; its `PT_LOAD` segments are mapped with
`mmap(MAP_PRIVATE | MAP_FIXED)` from the file, the tail of the last file
page is zeroed when memory extends beyond the file, and pages beyond the
file are mapped anonymously. A BSS-only segment includes its first page
even when its virtual address is not page aligned. A read-only segment
needing tail clearing is made writable only for that operation.
Libraries are placed from `0x7e0010000000` below `0x7e8000000000`, on at
least 1 MiB boundaries (larger `p_align` requirements are honored). Each
image reserves its holes with `PROT_NONE` and leaves a guard page after it.
Program headers are read separately, so they need not fit in the first
file page. Object records use a process-lifetime mmap arena and a linked
list; there is no fixed sixteen-object limit.

Symbols are looked up through SysV or GNU hash tables, in the order
program, libraries. GNU-only and dual-hash objects are supported. The
relocation types applied are `RELATIVE`,
`GLOB_DAT`, `JUMP_SLOT`, `64` and `COPY`; a `COPY` relocation copies the
library's data object into the program's own copy, which the lookup order
then makes the definition every object uses. All ordinary relocations
finish before any `COPY` relocation, so copied objects can contain
relocated pointers. `NONE` is a no-op. Local and non-default-visibility
definitions bind within their object; hidden and internal symbols do not
participate in external lookup. Absolute symbols are not rebased, and a
defined symbol at address zero is distinct from an unresolved symbol.
An undefined symbol that is
not weak stops the program with `ld.so: undefined symbol: name` and exit
status 127. Every relocation is applied before the program starts, since
the libraries are linked with `-z now`; there is no lazy binding and no
`dlopen`.

## Initialization and termination

The loader completes mapping and relocation before entering the program.
It passes `_dl_finalize` in `%rdx` and the MiniOS initialization callback
`_dl_initialize` in `%rcx`. `crt0.S` preserves those as arguments five and
four of `__libc_start`, respectively. Libc checks `AT_BASE` before using
them: a static program entered through `exec` has its entry address in
`%rcx`, because the kernel returns through `sysret`.

Libc initializes the main thread's control block, environment, program
name and standard streams, then registers the finalizer with `atexit`
before invoking constructors. This permits constructors to allocate,
access errno, use stdio, and register their own exit handlers.

The executable's `DT_PREINIT_ARRAY` runs first. An iterative depth-first
walk then runs dependencies before their dependents, marking visits to
avoid repeated initialization and to terminate dependency cycles. Each
object runs `DT_INIT` followed by its `DT_INIT_ARRAY` in array order.
Finalization records the actual initialization order and reverses it:
each object runs its `DT_FINI_ARRAY` backwards, then `DT_FINI`. Ordinary
atexit handlers run before the loader finalizer; stream flushing follows
finalization. `_exit` and `_Exit` bypass all handlers. Static executables
use the linker's preinit/init/fini array boundaries directly in libc.

## Validation and memory protection

The loader checks ELF class, byte order, machine, version, header sizes,
file bounds, segment sizes and alignment, overlapping load pages and
address arithmetic. Dynamic tables must terminate within their segment.
String, symbol, hash and relocation tables must fit in readable load
segments; symbol names must terminate within `DT_STRSZ`. Hash lookups
have bounded traversal, including cycle detection for SysV chains.

Relocation writes must fit in writable load segments. COPY sources must
be readable and at least as large as their destinations. Initializer and
finalizer targets must lie in executable segments. After relocation,
complete pages covered by `PT_GNU_RELRO` become read-only; a final partial
page remains writable because ordinary data can share it. Linkers must
emit a RELRO segment for this protection to apply.

Invalid objects produce an `ld.so:` diagnostic and exit status 127.
Unsupported TLS segments, text relocations, REL/RELR relocation tables,
symbol-version requirements and IFUNC symbols are rejected. This remains
a MiniOS startup loader, without lazy binding, dlopen, ELF TLS, symbol
versioning, IFUNC resolution, or alternate library search paths.

## Thread local storage

The loader does not implement TLS: no loaded object may carry a `PT_TLS`
segment. The one thread local variable of the C library, the buffer
`term_sgr` returned, became a field of the thread control block
(`sgr_sequence` in `struct pthread`), and the libc link rule checks the
shared object for a TLS segment.

## Profiler

Addresses inside a shared library were numeric to the profiler, which
resolves user addresses through the symbol table of `/bin/<name>`.
`/dev/maps` (`proc_format_maps` in `sched/proc.c`) lists the file backed
regions of every process, one line with the pid, the start and end
addresses, the file offset of the start and the file's path; for that,
`vfs_open` now keeps the canonical path of regular files in `file.path`
as it did for directories. `prof_symtab_add_maps` in the libc profiler
support reads the lines of one process, loads the symbol table of each
library under `/lib` once, and attaches the regions as modules of the
program's table; `prof_symtab_lookup` resolves an address inside a
module by its file offset, which is the link address in the text
segment of a shared object. `prof` and `sysmon` attach the modules after
loading a program's table.

## Tests

`tests/cases/dynlink` runs `/bin/dyntest`: the C library is mapped above
4 GiB and the program below 16 MiB, `environ`, `optind`, `errno` and the
standard streams are shared with the library, `qsort` calls back into the
program, and `exec` of another dynamically linked program succeeds.
`user/ld/tests/fixtures.mk` also builds:

- A chain of twenty DSOs using GNU-only and dual hash tables. Its event
  sequence checks preinit, dependency order, legacy init/fini hooks, array
  priorities, reverse finalization, and `_exit` bypass. Constructors use
  malloc, errno and stdio. An undefined weak symbol and an absolute symbol
  with value zero exercise lookup corner cases.
- A static version checking libc's array handling and the `AT_BASE` guard.
- Twenty-nine ELF fixtures generated from a linked DSO and executable by
  `fixtures.py`. Working cases cover SysV-only and GNU-only hashes, COPY
  of relocated pointers, unaligned BSS-only segments and program headers
  after the first file page. Rejection cases require normal exit 127 and
  a diagnostic; a crash cannot satisfy them. A separate child attempts a
  RELRO write and must terminate with SIGSEGV.

`tests/cases/profile` looks up `qsort` inside the shared C library through
`/dev/maps`. The shell kernel tests poll the free page count after the
shell exits, since the release of the file mappings of a program needs
more than one grace period.
