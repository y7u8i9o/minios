# Toolchain and global build settings shared by every Makefile.

# A native x86_64 Linux GCC/binutils toolchain produces the same freestanding
# ELF binaries as the prefixed cross toolchain.  macOS still needs cross tools
# because its native compiler and linker target Mach-O.
HOST_OS   := $(shell uname -s)
HOST_ARCH := $(shell uname -m)
ifeq ($(origin CROSS),undefined)
  ifeq ($(HOST_OS)-$(HOST_ARCH),Linux-x86_64)
    CROSS :=
  else
    CROSS := x86_64-elf-
  endif
endif
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
NM      := $(CROSS)nm
OBJCOPY := $(CROSS)objcopy
GDB     := $(CROSS)gdb
HOSTCC  ?= cc
YACC    ?= yacc
HOSTCPPFLAGS ?= -D_POSIX_C_SOURCE=200809L
QEMU    ?= qemu-system-x86_64
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

KCFLAGS := -std=c17 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
           -O2 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls -fno-asynchronous-unwind-tables \
           -fno-strict-aliasing -fno-builtin \
           -Wall -Wextra -Werror -Wno-unused-parameter -Wmissing-prototypes \
           $(CONFIG_DEFS)
KASFLAGS := -g $(CONFIG_DEFS)
KLDFLAGS := -nostdlib -static -z max-page-size=0x1000 --no-dynamic-linker

AR      := $(CROSS)ar

# User space flags: static, no PIC, red zone allowed. M23 saves x87 and all
# 128-bit XMM registers. Keep AVX disabled until the kernel migrates from
# FXSAVE to XSAVE/XRSTOR and enables the matching XCR0 state components.
UCFLAGS  := -std=c17 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
            -msse2 -mfpmath=sse -mno-avx -ftree-vectorize -fvect-cost-model=dynamic \
            -O2 -g -fno-omit-frame-pointer \
            -fno-builtin -Wall -Wextra -Wno-unused-parameter
UASFLAGS := -g
ULDFLAGS := -nostdlib -static -z max-page-size=0x1000 -Wl,-Ttext-segment=0x400000

# Machine size and accelerator for `make run` live in tools/run.sh
# (QEMU_MEM, QEMU_SMP, QEMU_ACCEL, QEMU_AUDIO, see qemu.conf.example).
