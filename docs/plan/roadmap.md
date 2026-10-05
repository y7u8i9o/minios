# Roadmap from 0.5 to 1.0

An overview of the releases up to 1.0, proposed on 2026-10-05. Each
release gets a detailed plan with milestones in this directory when its
work starts. The plan of 0.5 is `release-0.5.0.md`. The fixed decisions
of `README.md` apply throughout: QEMU machines only (UTM included), no
IPv6, forwarding or TLS. Section 3 proposes one change to them.

## 1. Releases

| Release | Theme | Main content |
|---|---|---|
| 0.5 | More machines, live medium | NVMe, AHCI with CD drives, USB mass storage and hubs (D3 to D5 of `drivers.md`); kernel support for `#!` scripts; a live medium that boots into the desktop and offers the installer |
| 0.6 | Virtual machine integration | folders shared with the host through virtio-9p or virtiofs; the desktop resolution follows the size of the QEMU or UTM window through the display events of virtio-gpu; clock synchronisation over NTP; a virtio memory balloon; power off and restart through ACPI on both architectures |
| 0.7 | Devices and desktop services | e1000e and Intel HD Audio (D6, D7); notifications; a screen locker; workspaces and window snapping; an archive manager for tar and gzip; a PDF viewer; a trash and file search in Files |
| 0.8 | Developer platform | the POSIX gaps of `/bin/sh` and the libc; a `vi`; `cron`; `ptrace` with `strace`; core dumps; minios builds its own packages inside minios with tcc and make; a ports tree for third-party software in the manner of `user/ports` |
| 0.9 | Robustness | a host harness for the compositor and the other user programs; a scheduled run of the whole test suite; file system repair inside minios; recovery after a crash during a package update |
| 0.10 | Stabilisation | feature freeze; translations reviewed by native speakers; keyboard access to every control; a user manual; performance work on boot time and memory |
| 1.0 | Stable release | the criteria of section 2 met |

## 2. Criteria for 1.0

1. The whole test suite passes on x86_64 and aarch64 and runs on a
   schedule.
2. POSIX shell scripts run unchanged, after a conformance pass over `sh`
   and the core utilities.
3. minios boots and installs on the default QEMU machines without virtio
   devices and in UTM.
4. The system call ABI, the library ABIs and the package format are
   stable, with an upgrade path from the last 0.x release.
5. No known defect loses data.

## 3. Decision of the owner

TLS is outside the fixed decisions. Without it minios cannot use HTTPS:
no package repositories on the web and no access to current web servers.
The proposal is a port of an existing library, BearSSL or mbedTLS, with
HTTPS in `http` and in the package repositories, in release 0.8. It enters
the roadmap only when the owner changes the decision in `README.md`.

## 4. After 1.0

MicroPython; an SSH client and server; a debugger; a version control
tool; a text web browser, which needs TLS; a small HTTP server; suspend to
disk through the swap partition; encryption of the data volume.

## 5. Release rules

- A release passes `make release` on x86_64 and aarch64.
- The owner boots each release medium in UTM on aarch64.
- Work that misses a release moves to the next one and does not delay it.
- 1.0 follows when its content is complete, not after a fixed number of
  releases. Releases after 0.9 continue as 0.10, 0.11 and so on.
