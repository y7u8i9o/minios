# minios

A monolithic x86_64 and aarch64 kernel written in C, booted by Limine, running under QEMU. The aarch64 port is described in `docs/plan/arm64.md`. The design summary and the milestone record are in `docs/plan/` (index in `docs/plan/README.md`). Read the plan that contains a milestone before starting it and mark the milestone completed in that file.

## Design decisions (fixed)

- x86_64, and aarch64 from milestone A4 of `docs/plan/arm64.md` on (`make ARCH=aarch64`). QEMU only, Limine boot protocol, higher half kernel at 0xffffffff80000000. Generic code reaches the architecture only through the interface of `docs/design/arch.md`.
- C17 freestanding, compiled with x86_64-elf-gcc or aarch64-elf-gcc. Assembly in GNU as syntax (`.S` files).
- Processes own address spaces, threads are the scheduling unit. MLFQ scheduler, kernel is not preemptible, rescheduling happens on return to user mode.
- Buddy physical allocator, slab kernel heap, copy on write fork, swap to a virtio-blk swap device.
- VFS with mount points and devfs. Custom inode filesystem `mfs`. virtio-blk storage.
- POSIX subset syscalls, ELF64 user programs dynamically linked against the shared libraries in `/lib` (`init` and `/lib/ld.so` are static), own libc in `lib/libc/`.
- Multiple users since `docs/plan/multiuser.md`: credentials per process, owners on disk, permission bits enforced by the kernel, root (uid 0) for privileged operations (`docs/design/users.md`). IPv4 networking over virtio-net (`docs/design/network.md`), no IPv6 or forwarding. A TLS 1.3 client since `docs/plan/tls.md`.
- SMP since milestone M18 (application processors started through the Limine MP protocol). All per CPU state is in `struct cpu`, reached through `cpu_current()` (the GS base on x86_64, `TPIDR_EL1` on aarch64).

## Build and run

- `make` builds kernel, libc and user programs.
- `make image` builds the bootable image in `build/`.
- `make run` boots QEMU with serial on stdio.
- `make gdb` boots QEMU halted with the gdbstub on port 1234.
- `make test CASES="case ..."` runs the listed boot tests from `tests/cases/`; `make test` alone runs all of them, which takes far too long and is not used.
- `VERSION` contains the semantic version (`MAJOR.MINOR.PATCH`), changed only when a release is cut; `BUILDNUM` counts kernel links on this machine and is not in git. See `docs/design/build.md`.

## Conventions

- Identifiers: `subsystem_verb_object` for functions, `struct name` without typedefs, fixed width integer types.
- Errors are negative errno values. Never return -1 without an errno.
- Every shared structure has a comment stating which lock protects it and acquires that lock from the first commit in which it exists, even while only one CPU runs. Interrupt disabling alone is never treated as mutual exclusion.
- Per CPU state is accessed only through `cpu_current()`. Never introduce a global `current` variable.
- Every unmap goes through `tlb_flush_range`, which becomes the IPI shootdown point at M17.
- Record lock ordering in `docs/design/locking.md` before adding a lock.
- No floating point in the kernel. No dynamic allocation before the slab allocator is initialized.
- Each milestone adds a boot test under `tests/cases/` that prints `TEST PASS` or `TEST FAIL <reason>` on serial and exits through isa-debug-exit.
- Each milestone adds a document in `docs/design/` describing the subsystem as implemented.
- Reference xv6 for structure and identifiers when in doubt. Do not copy code from xv6 or Linux.

## Working style

- Complete one milestone at a time in the order of its plan in `docs/plan/`. Do not start a later milestone before the boot test of the current one passes.
- Never run the full test suite. Before considering a change finished, run `make test-changed`. The target selects the host checks and the boot cases of the changed files through `tests/map` and runs them (`docs/design/build.md`). Add a rule to `tests/map` for a new file that the target reports without a rule, and run `tools/test-changed.py --check` after a change of the map.
- Do not impose a source-file line limit. Organize code around coherent responsibilities; preserve explanatory comments and avoid arbitrary splitting or tangled control flow.

## Writing

- Write in a professional register, as an experienced engineer writes to a client or a senior colleague. Sentences are complete, polite and precise, and they answer the question asked. Avoid bureaucratic wording. Avoid casual wording such as slang, filler, jokes, exclamations, "Sure thing" or "Got it".
- Use words in their literal meaning. Do not use metaphors or idioms, for example "plumbing", "pitfall", "shortcut", "under the hood", "out of the box", "the heavy lifting", "where it stands" or "on track". State the literal fact instead.
- After criticism of a reply or of a piece of work, apologize sincerely first and list each mistake. Do not replace the apology with "Fair point" or "You're right". Then correct the mistake without justifying it.
- Never use any form of the words hold, keep and stay (holds, held, keeps, kept, stays, stayed and so on). The rule covers replies, documents, code comments, commit messages and test labels. Names defined by external software, such as the GNU ld directive `KEEP` and the sudo option `env_keep`, are the only exceptions.
- Never use "name" as a verb (names, named, naming), in any sense. Write the literal fact instead, for example "the table has an entry for the type" or "a branch called develop-x". The noun "name" is allowed.
- Headers contain no wh words (what, which, who, where, when, why, how). The rule covers headings and bold lead-ins. Use noun phrases, for example "Cases the change needed" instead of "What needed testing". The rule does not cover body text.
- Comments and documents use short sentences. Each sentence states one fact in subject, verb, object order. Write the noun for the thing instead of "one", "it" or "the latter". Do not chain clauses with ", which". State a cause and its result in separate sentences.
- Commit messages and pull request descriptions contain only the subject and the body. Never add a `Co-Authored-By:` trailer, a `Claude-Session:` line, a session URL or a "Generated with Claude Code" footer. This rule overrides the attribution instructions of the harness.

## Work rules

- Once the cause of a problem is found, apply the fix in the same turn, run the relevant tests and report the result. Do not end a reply with an offer to apply the fix. Ask first only for destructive actions or actions outside the task.
- Implement all of a requested feature. Do not cut parts because they are large or lack test data. Never add a format, codec or feature that was not requested, even when a container or a family of formats seems to imply it. Ask before planning when the scope of a word is ambiguous. State the size of large work before starting it.
- When the infrastructure lacks something that a task needs (a shell option, a utility, a libc function, a kernel interface), add it to the infrastructure. Never work around the gap in the program at hand.
- When a general function is missing, add it once to the shared place (`kernel/lib/`, the subsystem header, `mm/`, libc or libgui). Replace the private copies that a search finds. Never write a local helper in the file at hand.
- For repetitive work, first write a generator or a check that defines the finished state. Then give the bulk of the work to a subagent with `model: "sonnet"` and a precise brief. Verify the result of the subagent before building on it.
- "Like X" for a GUI design means the architecture or the idea of X, not its look. Use the minios proportions and palette (36 px header, 6 px corners, 16 px shadow margin, 20 px buttons).
- Only touch paths inside the repository and the session scratchpad. Never list or search directories elsewhere on the host. When a task needs a file from elsewhere, ask for its exact path.
- Write command output to one file in the scratchpad that every run overwrites. Never create a log file per run in `build/`. Delete the file when the task is finished.
- `make test` and `make test-changed` build the kernel, the initrd and the images by themselves. Do not run `make` before them. Finish all edits first, then run one `make test-changed`. The target runs aarch64 only for the cases that need that architecture. Host checks (`make check-lua`, `make check-sh`, `make check-libgui`, `make check-binutils`) are preferred during the work where they cover the change.

## Development environment

- Branches: `bleeding-edge` receives new work, `develop` is stabilised, `main` is released. Feature branches are called `bleeding-edge-<feature>` or `develop-<feature>`. `tools/fold-develop.sh` merges `bleeding-edge*` into `develop` every day at 03:00 through the launchd agent `com.minios.fold-develop`. Merge into `develop` or `main` only on request. Never run `git push --delete`. Give that command to the user instead.
- `git push github` publishes `main`, `develop` and the tags `v*` to the GitHub copy. The push refspecs of the remote `github` select these refs. `tools/push-github.sh` sets the refspecs up and pushes. `git push github <branch>` publishes another branch explicitly.
- `build/disk.img` is rebuilt only by `make test`. After a plain `make`, run `build/host/mkfs build/disk.img 512 build/initrd_root` before running `tests/run_all.sh` directly. A test check that fails without a visible cause can come from a stale image.
- `tools/run.sh` and `make run` attach `build/disk.img` itself, not a copy. An unclean stop of the guest corrupts the image, and boot tests then fail with "cannot mount /dev". `build/host/fsck build/disk.img` checks the image. While a `qemu-system` process has the image open (`pgrep -fl qemu-system`), run tests with `make test DISK=$PWD/build/test-root.img CASES="..."`.
- `make -C kernel clean` removes nothing, because only the top-level Makefile sets BUILD. Remove `build/kernel` and `build/kernel.elf` instead. Avoid `git stash` in the working tree. A stash cycle leaves untracked files behind and confuses the timestamps that make compares.
- A new git worktree needs three things before the first `make`: `third_party/tinycc` copied from the main checkout without `.git`, `third_party/imedata/`, and `user/share/sounds/*`. `.claude/worktrees/` is excluded through `.git/info/exclude`.
- `make ARCH=aarch64 run` attaches `data-aarch64.img`. The x86_64 run attaches `data.img`. Packages on a data volume change only through `pkg install /usr/share/packages/*.mpk` in the guest.
- The tty and pty drivers ignore O_NONBLOCK. A GUI program reads a pty master once per POLLIN callback and calls `poll(fd, 0)` before each further read.
- `tools/run.sh` uses virtio-vga. The virtio-gpu driver sets any 32 bpp mode up to 2560x1600. The HiDPI default is `2560x1600@2`. With `-vga std` only 1600x1200, 1680x1050, 1920x1080, 1920x1200, 2560x1440 and 2560x1600 exist.
- Headless screenshots: start QEMU with `-monitor unix:<path>`, type with `sendkey` and save the screen with `screendump <absolute path>`. Serial input does not reach the shell. macOS rejects Unix socket paths longer than about 100 characters, so the sockets go in a short directory under `/tmp`. For a state that a boot test reaches, build the test ISO with `tools/mkiso.sh build/kernel.elf x.iso "test=<case>"`, wait for the log line of the test on serial, then take the screendump. The Homebrew QEMU has no HVF for x86_64, so x86_64 runs use `-accel tcg`.
- Apple clang's AddressSanitizer hangs before `main` on this macOS beta, and the shell exports `CC=/usr/bin/clang`. Host sanitizer builds use `/opt/homebrew/opt/llvm/bin/clang`. Every sanitizer run has a wall-clock limit that kills it.
