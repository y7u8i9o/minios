# Toolchain and global build settings shared by every Makefile.

CROSS   ?= x86_64-elf-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
NM      := $(CROSS)nm
OBJCOPY := $(CROSS)objcopy
GDB     := $(CROSS)gdb
HOSTCC  ?= cc
QEMU    ?= qemu-system-x86_64
XORRISO ?= xorriso

# Build time configuration. Override on the make command line, for example
# `make CONFIG_LOCKDEBUG=0`.
CONFIG_TESTS      ?= 1   # compile kernel self tests, selected with test=<name>
CONFIG_PANIC_EXIT ?= 1   # panic exits QEMU through isa-debug-exit instead of halting
CONFIG_LOCKDEBUG  ?= 1   # spinlock owner tracking and misuse detection
CONFIG_SLABDEBUG  ?= 1   # slab redzones and poisoning on free
CONFIG_LOG_LEVEL  ?= 1   # compile time klog threshold: 0 debug, 1 info, 2 warn, 3 error

CONFIG_DEFS := -DCONFIG_TESTS=$(strip $(CONFIG_TESTS)) \
               -DCONFIG_PANIC_EXIT=$(strip $(CONFIG_PANIC_EXIT)) \
               -DCONFIG_LOCKDEBUG=$(strip $(CONFIG_LOCKDEBUG)) \
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

# User space flags: static, no PIC, red zone allowed. SSE and x87 are
# available since M23 saves the FPU state on context switches.
UCFLAGS  := -std=c17 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
            -O2 -g -fno-omit-frame-pointer \
            -fno-builtin -Wall -Wextra -Wno-unused-parameter
UASFLAGS := -g
ULDFLAGS := -nostdlib -static -z max-page-size=0x1000 -Wl,-Ttext-segment=0x400000

QEMU_MEM   ?= 512M
# Use the macOS hypervisor framework when this QEMU build offers it.
QEMU_ACCEL ?= $(shell $(QEMU) -accel help 2>/dev/null | grep -q '^hvf$$' && echo hvf || echo tcg)
QEMU_SMP   ?= 4
QEMU_FLAGS := -M q35 -accel $(QEMU_ACCEL) -m $(QEMU_MEM) -smp $(QEMU_SMP) -serial stdio -no-reboot
