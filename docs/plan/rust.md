# Rust in the kernel

The owner decided on 2026-10-30 to introduce Rust into the kernel. Rust
does not replace the kernel. Rust code takes selected subsystems, first
the ones that parse untrusted data behind a narrow interface. The kernel
may use a small number of external crates, vendored into `third_party/`
and approved by the owner one at a time. Each milestone ends with its
tests and is marked completed here.

The design decisions and conventions of `CLAUDE.md` date from the start of
the project. This plan revises the ones that conflict with Rust and
retains the technical constraints.

## Technical constraints

These constraints follow from the hardware and the memory layout. They
apply to C and Rust alike.

- The x86_64 kernel links at 0xffffffff80000000. Rust code uses
  `-C code-model=kernel` and `-C relocation-model=static`, as the C code
  uses `-mcmodel=kernel` (`toolchain.mk`).
- The kernel does not save the FP and SIMD registers on entry. Rust code
  uses the targets `x86_64-unknown-none` (no SSE, no red zone) and
  `aarch64-unknown-none-softfloat` (no FP registers). The aarch64 C code
  uses `-mgeneral-regs-only -mno-outline-atomics -mcmodel=small`, and the
  Rust code generates inline atomics with the small code model as well.
- No allocation happens before the slab allocator is initialized.
- A Rust panic ends in the panic path of the kernel. The boot tests then
  see the same serial output as for a C panic.
- `panic=abort`. The kernel has no unwinder.

## Revised conventions

| Convention of `CLAUDE.md` | Revision | Reason |
|---|---|---|
| C17 freestanding only | C17, and Rust (`no_std` with `core` and `alloc`) for selected subsystems | The decision of 2026-10-30 |
| Build with make only | Cargo builds one static library from `kernel/rust/`, and make links the library into the kernel | rustup ships prebuilt `core` and `alloc` for both targets, so stable cargo works. Cargo manages the modules and the vendored crates |
| Negative errno in every function | `Result<T, Errno>` inside Rust, negative errno only at the boundary to C | The `?` operator replaces manual error checks |
| A comment that states the protecting lock | `SpinLock<T>` and `SleepLock<T>` contain the data, and only the guard gives access | The compiler checks the protection. The lock order still goes into `docs/design/locking.md` |
| `subsystem_verb_object`, no typedefs | Rust naming: modules, `CamelCase` types, methods. Functions exported to C retain the C form | The C rule replaces namespaces. Rust has namespaces |
| Per CPU state through `cpu_current()` | Unchanged, behind a safe wrapper | GS and `TPIDR_EL1` are the same for both languages |

## Milestones

### F1. Toolchain and build

- `rust-toolchain.toml` at the top of the repository fixes the version
  (1.98.1 at the time of writing) and the targets `x86_64-unknown-none`
  and `aarch64-unknown-none-softfloat`.
- `kernel/rust/Cargo.toml` defines the crate `minios_kernel` as a
  `staticlib` with `panic = "abort"` in every profile.
  `kernel/rust/.cargo/config.toml` sets the code generation flags of each
  target from the constraints above.
- The kernel Makefile runs `cargo build --release --offline --locked` with
  the target directory under `$(BUILD)/rust/` and links
  `libminios_kernel.a` into `kernel.elf`. `--offline` and `--locked`
  forbid network access during the build. The linker script places the Rust sections
  with the C sections.
- `make` checks for the toolchain and the targets, and prints the rustup
  command when one is missing.
- `tests/map` gains rules for `kernel/rust/**` and `rust-toolchain.toml`.
- Test: the boot case `rust_hello` calls an exported Rust function on
  x86_64 and on aarch64. The C test checks the return value.
- Document: `docs/design/rust.md` (build, targets, flags, linking).

### F2. The support crate

The modules of `minios_kernel` that every Rust subsystem uses:

- `bindings`: the declarations of the C interfaces. The script
  `tools/rust-bindings.sh` runs bindgen on a list of headers, and the
  generated file is committed, as the protocol code of libwire is. The
  build therefore needs no libclang. Only a change of a C interface needs
  bindgen on the host.
- `print`: the macros `kprint!` and `klog!` over the C logging functions.
- `alloc`: a `GlobalAlloc` over the slab allocator (`kmalloc`, `kfree`),
  with the alignment rules of the slab caches.
- `sync`: `SpinLock<T>` and `SleepLock<T>` over the C locks, with guards
  that release the lock on drop, and the interrupt state of the C spinlock.
- `cpu`: the current CPU and thread through `cpu_current()`.
- `error`: `Errno`, `Result<T>`, and the conversion to negative errno.
- `panic`: the `#[panic_handler]`. The handler passes the message and the
  location to the kernel panic.

Tests:

- `rust_support` allocates and frees `Box` and `Vec` values, takes the
  locks from two CPUs, and converts errors in both directions.
- `rust_panic` triggers a Rust panic and expects the panic line of the
  kernel.
- `make check-rust` runs `cargo test` on the host for the modules without
  kernel dependencies.

### F3. ISO 9660 in Rust

A Rust implementation replaces `kernel/fs/iso9660/iso9660.c` (865
lines). The file system is read-only, reads data from untrusted media, and
connects to the VFS through the operation tables `inode_ops` and
`file_ops`. The Rust code provides these tables as `extern "C"` functions.
The interface `fs/iso9660.h` and the mount through the VFS do not change.

- The features of the C version (`docs/design/iso9660.md`): ECMA-119,
  Rock Ridge (SUSP `SP`, `CE`, `ST`; RRIP `NM`, `PX`, `PN`, `SL`, `TF`,
  `RE`, `CL`), multi-extent files, and the inode numbers.
- The parsing of descriptors, directory records and system use entries
  works on byte slices and checks every length against the slice. The host
  check `make check-rust` parses the images of the boot cases and
  corrupted copies of them.
- The C version adapts parts of Limine (BSD 2-clause). The Rust version is
  written from ECMA-119 and RRIP. `docs/design/iso9660.md` records the
  origin of the new code.
- Tests: `iso9660`, `iso9660_plain` and `iso9660_root` pass unchanged,
  and the live medium boots (`live`). The C file is then removed.

## Later candidates

These subsystems also parse data from devices or from the host. Each one
becomes a milestone of its own after F3, on request.

- FAT, `kernel/fs/fat/` (1438 lines).
- The descriptor parsing of USB, `kernel/drivers/usb/`.
- The 9P messages, `kernel/fs/9p/` (1272 lines).

## Changes to `CLAUDE.md`

F1 changes the design decisions and the conventions of `CLAUDE.md`:

- Design decisions: "C17 freestanding, and Rust (`no_std` with `core` and
  `alloc`) for selected subsystems, built by cargo with the toolchain of
  `rust-toolchain.toml`."
- Build and run: the rustup targets as a requirement of `make`.
- Conventions: a Rust section with the revisions of the table above, and
  the rule that every external crate needs the approval of the owner and a
  vendored copy in `third_party/`.

## Size

About 300 lines for F1 (build, linker script, test), about 1200 lines of
Rust and the generated bindings for F2, and about 900 lines of Rust with
the host checks for F3, together with the documents.
