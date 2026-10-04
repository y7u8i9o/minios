# Toolchain and build settings for every Makefile of the project.

# ARCH is the target architecture, x86_64 (the default) or aarch64. The
# kernel compiles the code in arch/$(ARCH) and uses the headers in
# arch/$(ARCH)/include. See docs/design/arch.md and docs/plan/arm64.md.
ARCH ?= x86_64

# CROSS is the prefix of the compiler and binutils commands. On a Linux
# host of the same architecture as ARCH, the host GCC and binutils produce
# the same ELF files as a cross toolchain, so CROSS is empty there. On every
# other host, CROSS is $(ARCH)-elf-. macOS always needs the cross
# toolchain, because the compiler and linker of macOS produce Mach-O files.
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
# When only _POSIX_C_SOURCE is defined, the macOS headers do not declare
# the socket control message macros (CMSG_*) and the resource limit
# extensions. The host tests use these declarations, so _DARWIN_C_SOURCE is
# added on macOS. HOSTCPPFLAGS is used for host programs only, not for
# minios.
ifeq ($(HOST_OS),Darwin)
HOSTCPPFLAGS += -D_DARWIN_C_SOURCE
endif
QEMU    ?= qemu-system-$(ARCH)
XORRISO ?= xorriso

# Build configuration. Each value can be set on the make command line, for
# example `make CONFIG_LOCKDEBUG=0`.
CONFIG_TESTS      ?= 1   # compile the kernel self tests, a test is selected with test=<name>
CONFIG_PANIC_EXIT ?= 1   # a panic exits QEMU through isa-debug-exit instead of halting
CONFIG_LOCKDEBUG  ?= 1   # record the owner of each spinlock and detect wrong use of locks
CONFIG_LOCKSTAT   ?= 1   # count acquisitions, contention and locked time per lock name, in /dev/lockstat
CONFIG_SLABDEBUG  ?= 1   # add redzones to slab objects and fill freed objects with a pattern
CONFIG_LOG_LEVEL  ?= 1   # lowest klog level compiled in: 0 debug, 1 info, 2 warn, 3 error

CONFIG_DEFS := -DCONFIG_TESTS=$(strip $(CONFIG_TESTS)) \
               -DCONFIG_PANIC_EXIT=$(strip $(CONFIG_PANIC_EXIT)) \
               -DCONFIG_LOCKDEBUG=$(strip $(CONFIG_LOCKDEBUG)) \
               -DCONFIG_LOCKSTAT=$(strip $(CONFIG_LOCKSTAT)) \
               -DCONFIG_SLABDEBUG=$(strip $(CONFIG_SLABDEBUG)) \
               -DCONFIG_LOG_LEVEL=$(strip $(CONFIG_LOG_LEVEL))

# Compiler flags for each architecture:
#   KARCHFLAGS      flags for the kernel
#   UARCHFLAGS      flags for user programs
#   LDSO_ARCHFLAGS  additional flags for the dynamic loader /lib/ld.so
#   TCC_TARGET      the code generator of the bundled tcc
#   ARCH_USERLAND   "yes" if user programs are built for the architecture.
#                   With "no", the build and the boot tests use an empty
#                   initrd and no root disk.
ifeq ($(ARCH),x86_64)
# The kernel does not use the red zone. It uses the kernel code model,
# because it is linked in the higher half. It does not use SSE, MMX or x87
# registers, because it does not save these registers for its own use.
# User programs use SSE2. Since M23, the kernel saves the x87 and XMM
# registers of user threads. AVX is disabled, because the kernel saves
# registers with FXSAVE, which does not include the AVX registers. AVX
# requires that the kernel uses XSAVE/XRSTOR and sets the matching bits in
# XCR0.
KARCHFLAGS := -mno-red-zone -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-80387
UARCHFLAGS := -msse2 -mfpmath=sse -mno-avx
LDSO_ARCHFLAGS :=
TCC_TARGET := X86_64
ARCH_USERLAND := yes
else ifeq ($(ARCH),aarch64)
# The kernel uses only the general purpose registers. Atomic operations
# are compiled inline as LL/SC or LSE instructions. The atomic helpers of
# libgcc are not used, because they choose an implementation at run time
# from the auxiliary vector. With the small code model, the kernel
# addresses its whole image PC relative from its higher half address.
KARCHFLAGS := -march=armv8-a -mgeneral-regs-only -mno-outline-atomics -mcmodel=small
# User programs use the traditional TLS model, which calls
# __tls_get_addr, because the loader does not implement TLS descriptors.
# The loader is compiled without floating point registers, so its recovery
# buffer (_dl_setjmp) contains only the general purpose registers.
UARCHFLAGS := -march=armv8-a -mno-outline-atomics -mtls-dialect=trad
LDSO_ARCHFLAGS := -mgeneral-regs-only
TCC_TARGET := ARM64
ARCH_USERLAND := yes
else
$(error ARCH=$(ARCH) is not supported; see docs/plan/arm64.md)
endif

# Source paths in the debug information and in __FILE__ are written
# relative to the source tree or to the build directory. The binaries
# therefore do not contain the location of these directories on the build
# machine. The build directory can be outside the source tree, and it
# contains generated sources such as version.c.
PREFIX_MAP = $(if $(BUILD),-ffile-prefix-map=$(BUILD)/=build/) $(if $(TOP),-ffile-prefix-map=$(TOP)/=)

KCFLAGS := -std=c17 -ffreestanding -fno-stack-protector -fno-pic -fno-pie $(PREFIX_MAP) \
           $(KARCHFLAGS) \
           -O2 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls -fno-asynchronous-unwind-tables \
           -fno-strict-aliasing -fno-builtin \
           -Wall -Wextra -Werror -Wno-unused-parameter -Wmissing-prototypes \
           $(CONFIG_DEFS)
KASFLAGS := -g $(PREFIX_MAP) $(CONFIG_DEFS)
KLDFLAGS := -nostdlib -static -z max-page-size=0x1000 --no-dynamic-linker

AR      := $(CROSS)ar

# User code is compiled position independent, so the same object files can
# be linked into the shared libraries and into programs. User code may use
# the red zone.
UCFLAGS  := -std=c17 -ffreestanding -fno-stack-protector -fPIC $(PREFIX_MAP) \
            $(UARCHFLAGS) -ftree-vectorize -fvect-cost-model=dynamic \
            -O2 -g -fno-omit-frame-pointer \
            -fno-builtin -Wall -Wextra -Wno-unused-parameter
UASFLAGS := -g $(PREFIX_MAP)
# ULDFLAGS links a program at address 0x400000 against the shared
# libraries in build/lib. All relocations are applied when the program is
# loaded (docs/design/dynlink.md). ULDFLAGS_STATIC links a program without
# shared libraries, and it is used for init and for the loader.
# The layout options are given explicitly, because the default options
# differ between toolchains. On a Linux distribution, ld enables RELRO and
# separate code segments by default, and gcc links position independent
# executables by default. The x86_64-elf toolchain does none of this. With
# the explicit options, both toolchains produce the same layout, so the
# boot tests on macOS test the layout that a Linux host produces.
ULAYOUT  := -z relro -z separate-code
ULAYOUT_WL := -Wl,-z,relro -Wl,-z,separate-code
ULDFLAGS := -nostdlib -no-pie -z max-page-size=0x1000 -Wl,-Ttext-segment=0x400000 $(ULAYOUT_WL) \
            -Wl,--hash-style=sysv -Wl,-z,now -Wl,--as-needed -Wl,-dynamic-linker,/lib/ld.so -L$(BUILD)/lib
ULDFLAGS_STATIC := -nostdlib -static -no-pie -z max-page-size=0x1000 -Wl,-Ttext-segment=0x400000 $(ULAYOUT_WL)
# Shared libraries are linked by calling ld directly, because the gcc of the
# bare metal target does not pass -shared to the linker.
USOFLAGS := -shared -z now --hash-style=sysv -z max-page-size=0x1000 $(ULAYOUT)

# The memory size, the number of CPUs, the accelerator and the audio
# output of `make run` are set in tools/run.sh (QEMU_MEM, QEMU_SMP,
# QEMU_ACCEL, QEMU_AUDIO, see qemu.conf.example).
