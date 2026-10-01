# Toolchain and global build settings shared by every Makefile.

# ARCH selects the target architecture. The kernel builds arch/$(ARCH) and
# takes its architecture headers from arch/$(ARCH)/include
# (docs/design/arch.md). x86_64 is the only implemented architecture; the
# aarch64 port is planned in docs/plan/arm64.md.
ARCH ?= x86_64

# A native Linux GCC/binutils toolchain for ARCH produces the same
# freestanding ELF binaries as the prefixed cross toolchain.  macOS still
# needs cross tools because its native compiler and linker target Mach-O.
HOST_OS   := $(shell uname -s)
HOST_ARCH := $(shell uname -m)
ifeq ($(origin CROSS),undefined)
  ifeq ($(HOST_OS)-$(HOST_ARCH),Linux-$(ARCH))
    CROSS :=
  else
    CROSS := $(ARCH)-elf-
  endif
endif
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
NM      := $(CROSS)nm
OBJCOPY := $(CROSS)objcopy
READELF := $(CROSS)readelf
LIBGCC  := $(shell $(CROSS)gcc -print-libgcc-file-name)
GDB     := $(CROSS)gdb
HOSTCC  ?= cc
YACC    ?= yacc
HOSTCPPFLAGS ?= -D_POSIX_C_SOURCE=200809L
# Darwin hides socket ancillary-data macros and resource-limit extensions
# under strict POSIX visibility. Host tests use those native interfaces;
# this flag never reaches the freestanding MiniOS build.
ifeq ($(HOST_OS),Darwin)
HOSTCPPFLAGS += -D_DARWIN_C_SOURCE
endif
QEMU    ?= qemu-system-$(ARCH)
XORRISO ?= xorriso

# Build time configuration. Override on the make command line, for example
# `make CONFIG_LOCKDEBUG=0`.
CONFIG_TESTS      ?= 1   # compile kernel self tests, selected with test=<name>
CONFIG_PANIC_EXIT ?= 1   # panic exits QEMU through isa-debug-exit instead of halting
CONFIG_LOCKDEBUG  ?= 1   # spinlock owner tracking and misuse detection
CONFIG_LOCKSTAT   ?= 1   # per lock name acquisition, contention and hold time counters, /dev/lockstat
CONFIG_SLABDEBUG  ?= 1   # slab redzones and poisoning on free
CONFIG_LOG_LEVEL  ?= 1   # compile time klog threshold: 0 debug, 1 info, 2 warn, 3 error

CONFIG_DEFS := -DCONFIG_TESTS=$(strip $(CONFIG_TESTS)) \
               -DCONFIG_PANIC_EXIT=$(strip $(CONFIG_PANIC_EXIT)) \
               -DCONFIG_LOCKDEBUG=$(strip $(CONFIG_LOCKDEBUG)) \
               -DCONFIG_LOCKSTAT=$(strip $(CONFIG_LOCKSTAT)) \
               -DCONFIG_SLABDEBUG=$(strip $(CONFIG_SLABDEBUG)) \
               -DCONFIG_LOG_LEVEL=$(strip $(CONFIG_LOG_LEVEL))

# The architecture flags of the kernel and of user programs.
# Kernel: no red zone, the kernel code model for the higher half, and no
# SIMD or floating point registers (the kernel never saves them for
# itself). User: M23 saves x87 and all 128-bit XMM registers. Keep AVX
# disabled until the kernel migrates from FXSAVE to XSAVE/XRSTOR and enables
# the matching XCR0 state components. TCC_TARGET selects the backend of the
# bundled tcc.
ifeq ($(ARCH),x86_64)
KARCHFLAGS := -mno-red-zone -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-80387
UARCHFLAGS := -msse2 -mfpmath=sse -mno-avx
TCC_TARGET := X86_64
else
$(error ARCH=$(ARCH) is not supported; see docs/plan/arm64.md)
endif

KCFLAGS := -std=c17 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
           $(KARCHFLAGS) \
           -O2 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls -fno-asynchronous-unwind-tables \
           -fno-strict-aliasing -fno-builtin \
           -Wall -Wextra -Werror -Wno-unused-parameter -Wmissing-prototypes \
           $(CONFIG_DEFS)
KASFLAGS := -g $(CONFIG_DEFS)
KLDFLAGS := -nostdlib -static -z max-page-size=0x1000 --no-dynamic-linker

AR      := $(CROSS)ar

# User space flags: position independent code, so that the same objects go
# into the shared libraries and the programs; red zone allowed.
UCFLAGS  := -std=c17 -ffreestanding -fno-stack-protector -fPIC \
            $(UARCHFLAGS) -ftree-vectorize -fvect-cost-model=dynamic \
            -O2 -g -fno-omit-frame-pointer \
            -fno-builtin -Wall -Wextra -Wno-unused-parameter
UASFLAGS := -g
# Programs are linked at 0x400000 against the shared libraries in
# build/lib, with every relocation applied at load (docs/design/dynlink.md);
# ULDFLAGS_STATIC links a program on its own, for init and the loader.
# The layout options are stated rather than left to the linker's defaults,
# which differ: the ld of a Linux distribution enables RELRO and separate
# code segments and its gcc links position independent executables, while
# the x86_64-elf tools do none of this. Both toolchains therefore produce
# the same layout, and the boot tests exercise the one a Linux host builds.
ULAYOUT  := -z relro -z separate-code
ULAYOUT_WL := -Wl,-z,relro -Wl,-z,separate-code
ULDFLAGS := -nostdlib -no-pie -z max-page-size=0x1000 -Wl,-Ttext-segment=0x400000 $(ULAYOUT_WL) \
            -Wl,--hash-style=sysv -Wl,-z,now -Wl,--as-needed -Wl,-dynamic-linker,/lib/ld.so -L$(BUILD)/lib
ULDFLAGS_STATIC := -nostdlib -static -no-pie -z max-page-size=0x1000 -Wl,-Ttext-segment=0x400000 $(ULAYOUT_WL)
# Shared libraries are linked with ld directly: the compiler driver of the
# bare metal target does not pass -shared on.
USOFLAGS := -shared -z now --hash-style=sysv -z max-page-size=0x1000 $(ULAYOUT)

# Machine size and accelerator for `make run` live in tools/run.sh
# (QEMU_MEM, QEMU_SMP, QEMU_ACCEL, QEMU_AUDIO, see qemu.conf.example).
