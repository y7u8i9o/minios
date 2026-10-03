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

`toolchain.mk` compiles all user code with `-fPIC`. The archives and the
shared objects are built from the same objects. Each library Makefile links
`$(BUILD)/lib/<name>.so` with `ld -shared -z now --hash-style=sysv
-z relro -z separate-code -soname <name>.so` and names the libraries it depends on, so that the
loader finds them through `DT_NEEDED`; the compiler driver of the bare
metal target does not pass `-shared` on, hence the direct use of `ld`.
`libgcc.a` is linked into every shared object.

Programs link with `ULDFLAGS`: `-no-pie`, `-Ttext-segment=0x400000`,
`-dynamic-linker /lib/ld.so`, `-z now`, `-z relro`, `-z separate-code`,
`--hash-style=sysv` and `--as-needed`, against `-lgui -laudio -lwire -lfont libedit.a -lc`.
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

`user/ld/ld.c` and `user/ld/arch/$(ARCH)/start.S` build `/lib/ld.so`, a
freestanding position independent object linked with `-Bsymbolic`, hidden
visibility, `-fno-plt` and its own entry `_dl_start`; it uses no libc and
makes its system calls through an inline `syscall` or `svc`. The machine
number, the relocation types, the system call stub, the thread pointer and
the size of the recovery buffer are in `user/ld/arch/$(ARCH)/ld_arch.h`;
`ld.c` names the relocation types generically (`RELOC_RELATIVE`,
`RELOC_JUMP_SLOT` and so on). On aarch64 the program is entered with the
finalizer in `x0` and the initializer in `x1`; the lazy binding trampoline
takes the GOT slot that the AArch64 PLT pushed and preserves `x0` to `x8`
and `q0` to `q7`; user code is compiled with `-mtls-dialect=trad`, so the
loader needs no TLS descriptors. `_dl_main` receives the initial stack pointer
and returns the program's entry point; `start.S` enters it with the stack
pointer the kernel provided, the finalizer `_dl_finalize` in `rdx` (the
x86-64 ELF convention) and the initializer `_dl_initialize` in `rcx`.

Startup has three phases. The loader first applies its own relocations,
which are all `R_X86_64_RELATIVE`; the address of its dynamic section is
taken through a hidden symbol, so that no global offset table entry is
read before it is relocated. It then builds the dependency graph: the
program's `PT_DYNAMIC` is found through the headers named by `AT_PHDR`,
and the libraries named by `DT_NEEDED` are loaded breadth first, the
program's own libraries in order, then theirs, which gives the symbol
search order. Object records are allocated from a small mmap arena and form a list
without a fixed cap; the program record is first. The third phase,
initialization, runs later from the C library (below).

Every table an object names is checked before use. A range must lie
inside one `PT_LOAD` segment with the needed permission, not merely inside
the image; strings must be terminated inside `DT_STRTAB`; the SysV hash
table's indices and the GNU hash table's buckets and chains are validated
while the symbol count is derived from them; and the entry sizes of the
symbol and relocation tables must be the ELF64 ones. The loader refuses,
with a diagnostic, objects with `REL` or `RELR` relocations, symbol
versioning, text relocations, IFUNC symbols, a TLS segment whose image
is larger than its block or lies outside the load segments, a preinit
array in a library, a needed library name with a slash, overlapping or
misaligned load segments, and initializers outside executable segments.
An error prints `ld.so: what: name` to standard error and exits with
status 127.

A library is `/lib/<soname>`, or `/usr/local/lib/<soname>` when `/lib`
does not hold it, the directory of installed packages (`packages.md`), or
since U3 of the multiuser plan `$HOME/.local/lib/<soname>` of the user,
which a program with `AT_SECURE` never searches (`users.md`).
Its span is reserved with `PROT_NONE` at the
next address of an arena that starts at `0x7e0010000000` and ends at
`0x7e8000000000`, each library on a 1 MiB boundary or the alignment its
segments ask for, followed by an unmapped page; the mmap area below
`USER_MMAP_TOP` stays as it was. Each `PT_LOAD` segment is mapped
privately from the file, the tail of the last file page is zeroed, and
pages beyond the file are mapped anonymously, including the first page of
a segment without file bytes.

Symbols are looked up through the GNU hash table when an object has one
and through the SysV table otherwise, honouring hidden and internal
visibility, in the order program then libraries; a local or protected
definition binds inside its own object. The relocation types applied are
`RELATIVE`, `GLOB_DAT`, `JUMP_SLOT`, `64` and `COPY`. The TLS relocation types
`DTPMOD64`, `DTPOFF64` and `TPOFF64` are described below. Relocations
run in two passes, the ordinary ones of every object and then the `COPY`
relocations of the program, because a copied data object may itself hold
relocated pointers; a `COPY` relocation is refused when its source is
smaller than its destination or its owner is not the program. Every
relocation of an object linked with `-z now` (`DT_FLAGS` with
`DF_BIND_NOW`, or `DT_BIND_NOW`), which is how the build links every
program and library, is applied before the program starts. After
relocation the `PT_GNU_RELRO` range of every object is made read-only up
to its last page boundary. The range must start in a readable load
segment and may end in the padding of that segment's last page, because
GNU ld rounds its end up to a page boundary: a library without ordinary
writable data after its RELRO sections, such as `libwire.so`, has a RELRO
range longer than its writable segment. Every other table and relocation
target must lie within the memory size of one load segment.

Symbol lookup walks scopes. The global scope holds the program and the
libraries loaded at start in breadth first order, then any object opened
with `RTLD_GLOBAL`; every `dlopen` gives the object it loads and the
libraries loaded with it a local scope, searched after the global one
when those objects are relocated. Each object records its local scope;
the initial objects share the global scope as theirs.

## Lazy binding

An object without `DF_BIND_NOW` keeps its `JUMP_SLOT` entries pointing
into its own procedure linkage table, rebased for a shared object, and
the loader stores the object record in the second word of the table
named by `DT_PLTGOT` and `_dl_runtime_resolve` in the third. The first
call through an entry pushes the relocation index and the record and
reaches the trampoline in `start.S`, which preserves every register an
argument may travel in, including the eight vector registers of
floating point arguments, and calls `_dl_fixup`. That resolves the one
relocation under the loader lock, exactly as the eager pass would, and
stores the address in the slot, so the next call goes straight through;
the trampoline drops the two pushed words and continues to the target.
An undefined symbol met this way ends the process with the same
diagnostic as at startup. The `-z lazy` fixture below exercises the
path; the system itself is linked eagerly.

## The runtime interface

After relocating the initial objects the loader looks up the variable
`__dl_interface` of the C library and stores in it the address of its
interface record, `struct dl_interface` in `minios/dl.h`: the static TLS
size and alignment, and the functions behind `__tls_get_addr`, thread
creation and exit, `dlopen`, `dlsym`, `dlclose` and `dlerror`. The C
library fills in `malloc` and `free` once its heap works; the loader
uses them for the vectors and blocks of dynamic TLS, and its own mmap
arena for everything else. A static program has no loader and leaves the
pointer NULL; libc then handles the program's own TLS segment and fails
`dlopen` with a message.

The loader's runtime entries take one recursive futex lock, `dl_lock`,
since a constructor run by `dlopen` may itself call `dlopen` or reach a
lazily bound entry; the lock is taken before libc's `malloc` lock and
never the other way around, because nothing in `malloc` uses thread
local storage. A failure inside `dlopen` or `dlsym` does not exit: the
entries set a recovery point (`_dl_setjmp` in `start.S`) and `die`
returns to it with the diagnostic stored as the `dlerror` text, after
which whatever the failed call mapped is unmapped.

## dlopen

`dlopen(name, mode)` under the lock: `NULL` returns the program, whose
handle searches the global scope (`RTLD_DEFAULT`). A name whose
basename matches a loaded object returns that object with one more
reference, promoted to the global scope when `RTLD_GLOBAL` is given.
Otherwise the object is mapped, from the library directories by soname
or from the path when the name holds a slash. The loader stores a copy
of the name, because the caller may reuse its buffer for the next
`dlopen` (the names of `DT_NEEDED` entries remain in the string tables of
their objects). The libraries it needs
that are not loaded yet follow breadth first; the new objects and the
loaded ones they depend on form the group's local scope. The new
objects are relocated against the global scope and that local scope,
lazily unless `RTLD_NOW` or their own `DF_BIND_NOW` says otherwise, get
their RELRO protection, and only then join the list of loaded objects,
so that a lookup from another thread never sees a half built record.
Every member of the group gains a reference, `RTLD_GLOBAL` adds the
members to the global scope, and the initializers of the new objects
run in dependency order through the same depth first walk as at start,
skipping objects initialized earlier. A failure anywhere releases the
mappings of the new objects and returns `NULL`.

`dlsym(handle, name)` searches the handle's object and then its local
scope, or the global scope for `RTLD_DEFAULT`; a TLS symbol yields the
calling thread's copy. `dlclose(handle)` drops one reference from every
member of the handle's scope; the members left without references, and
never an initial object, run their finalizers in the reverse of their
initialization order (they are removed from the finalization chain that
`exit` walks), leave the global scope and the object list, are unmapped
and, when they had TLS, give up their vector slot. A thread that used
the storage of an unloaded object keeps that block until its exit or
until the slot is reused, when the generation stored with the block no
longer matches the module's and the block is replaced. Object records
return to a free list. Package libraries in `/usr/local/lib` are found
by soname like the system ones.

## Initialization and finalization

`crt0.S` passes the two callbacks from the loader to `__libc_start`, which
trusts them only when the auxiliary vector carries a non-zero `AT_BASE`,
since `sysret` leaves the entry address in `rcx` for a static program.
After the thread control block, `environ` and the standard streams are
set up, libc registers the finalizer with `atexit` and calls the
initializer. `_dl_initialize` runs the program's preinit array, then the
`DT_INIT` function and the `DT_INIT_ARRAY` entries of every object in
dependency order, a depth first walk over `DT_NEEDED` that visits each
object once; the order in which objects finished initializing is recorded
and `_dl_finalize` runs the `DT_FINI_ARRAY` entries and `DT_FINI` in
reverse, removing each record before calling into it, so that a recursive
`exit` cannot finalize an object twice. `_exit` bypasses finalizers. A
static program runs its own arrays from the linker's start and end
symbols.

## Thread local storage

The thread pointer addresses the thread control block, the two words
`struct dl_tcb` names: a pointer to the thread's `struct pthread` and the
dynamic thread vector. `minios/dl.h` states the two layouts with
`dl_tls_block`, `dl_tls_place` and `dl_tls_tprel`. On x86_64 (variant II,
the FS base) the control block is the `struct pthread` itself, and the
blocks of the objects loaded at start lie below it at fixed offsets: the
program's block ends at the thread pointer, its offset being its size
rounded up to its alignment, and each further initial object's block
follows below it. On aarch64 (variant I, `TPIDR_EL0`) the control block is
16 bytes after the `struct pthread`, and the blocks lie above it: the
program's block starts at the first offset after the 16 bytes that its
alignment allows, and each further block follows. In both layouts the
program's offset is what the linker assumed when it resolved the program's
own local-exec accesses. The loader computes this
static layout once the initial objects are loaded and publishes its size
and alignment; libc reserves the space below every control block, of
the main thread (whose block moves from static storage to a mapping when
the process has TLS) and of every thread `pthread_create` starts, and
asks the loader's `tls_setup` to copy the images and clear the rest
before the thread runs.

The loader writes the address of the object's `struct dl_tls_module`
record, rather than a small integer, as the module id of a `DTPMOD64`
relocation, so `__tls_get_addr` in libc, which the general dynamic model
calls with a module id and an offset, reaches the record directly: for
a static module it returns the module's block (`dl_tls_block`) plus the
variable's offset without entering the loader, for a dynamic one it
calls the loader's slow path. `DTPOFF64` stores the variable's offset in
its block and `TPOFF64`, the initial-exec model, the offset from the
thread pointer, which exists only for a static module; a relocation of
that kind against an object loaded by `dlopen` is refused. A relocation
with symbol index zero refers to the object's own module with the addend
as the offset, as the linker emits for local variables.

An object loaded by `dlopen` gets a slot in the dynamic thread vector
and a generation number. The slow path grows the calling thread's
vector to cover the slot, allocates and initializes a block on the first
access, or again when the slot's generation changed because a later
object reuses it after the earlier one was closed, and returns the
address. `pthread_exit` frees the blocks and the vector through the
loader's `tls_free`. A static program keeps one module, its own `PT_TLS`
segment read from `AT_PHDR`, handled by libc's `tls.c` without the
loader.

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

`tests/cases/dlopen` runs `/bin/dltest`, linked with `--export-dynamic`
against `libldtls.so`, a library with thread local variables in every
model (general dynamic, initial-exec and local dynamic) and a variadic
function taking doubles. The program checks its own local-exec
variables and the library's from the main thread and from a worker
thread created before anything else, that the two threads have separate
blocks initialized from the images, that `dlopen` of a missing library
and of an executable fail with their diagnostics and leave the process
running, that a library loaded at start is found and its symbols
resolve to the loaded copies, and then loads `libldplugin.so`, which no
program links against, needs `libldplugdep.so`, calls back into the
program through the exported `dltest_events` and into `libldtls.so`, and
has TLS of its own: the dependency initializes before the plugin,
`dlsym` finds the plugin's functions, its dependency's symbols and the
calling thread's copy of a TLS variable, `RTLD_LOCAL` keeps it out of the
global scope until a second `dlopen` with `RTLD_GLOBAL` promotes it, the
worker thread created before the load gets its own block, the first
`dlclose` drops a reference and the second unloads with the finalizers
in reverse order, and a third `dlopen` gets a fresh copy whose static
data and TLS start over, in the worker too, and whose lazily bound
calls work. `/bin/ldlazy`, linked with `-z lazy`, proves through
`DT_PLTGOT` that its jump slots point into its own table before the
first call and into the libraries after, and that a variadic call with
doubles survives the resolver.

`tests/cases/dynlink` runs `/bin/dyntest`. The program checks that the C
library is mapped above 4 GiB and the program below 16 MiB, that
`environ`, `optind`, `errno` and the standard streams are shared with the
library, that `qsort` calls back into the program, and that `exec` of
another dynamically linked program succeeds. It then runs the fixtures
that `user/ld/tests/fixtures.mk` builds: `ldlifecycle`, linked against a
chain of twenty libraries that alternate between GNU only and dual hash
tables, whose constructors and destructors write one character each, so
that the captured output proves the dependency order of initialization
and the reverse order of finalization, `DT_INIT` and `DT_FINI` beside the
arrays, the preinit array running before the libraries, an absolute
symbol with value zero, a weak undefined symbol, and `_exit` skipping the
destructors; `ldstatic`, the same program linked statically; and the
rejection fixtures. `user/ld/tests/fixtures.py` takes one linked library
and one executable and produces, by changing headers and dynamic tables
of the copies, a set of cases with a manifest under
`/usr/share/ldtests/cases` naming the expected exit status and
diagnostic: a relocation into code, a wrong relocation entry size, an
unsupported relocation type, GNU only and SysV only hash tables, a zero
filled segment starting inside a page, program headers after the first
page, an initializer outside code, a symbol name outside the string
table, a misaligned load segment, a missing library, a TLS segment with
an image larger than its block, a TLS image outside the load segments,
and a write into a RELRO page, which must end in `SIGSEGV`. The
lifecycle programs, dynamic and static, also check a thread local
variable of the program itself. A loader crash on a rejection
case is a test failure, since the expected status is 127 and a signal is
reported separately.

`tests/cases/profile` looks up `qsort` inside the shared C library through
`/dev/maps`. The shell kernel tests poll the free page count after the
shell exits, since the release of the file mappings of a program needs
more than one grace period. The cases of every program, the shell, the
filesystems, the graphical session and the audio stack were run after
the change.
