include toolchain.mk

TOP      := $(CURDIR)
BUILD    := $(TOP)/build
KERNEL   := $(BUILD)/kernel.elf
ISO      := $(BUILD)/minios.iso
INITRD   := $(BUILD)/initrd.tar
DISK     := $(BUILD)/disk.img
DISK_MB  ?= 512
SWAP     := $(BUILD)/swap.img
SWAP_MB  ?= 64
LIMINE   := $(BUILD)/host/limine
GENSYMS  := $(BUILD)/host/gensyms
MKFS     := $(BUILD)/host/mkfs
FSCK     := $(BUILD)/host/fsck
MKFAT    := $(BUILD)/host/mkfat

export TOP BUILD KERNEL LIMINE GENSYMS INITRD DISK MKFS FSCK MKFAT SWAP

.PHONY: all kernel libc libfont libwire libaudio libgui user initrd disk image run gdb test test-kvm check clean tools $(DISK)

all: kernel libc user

tools: $(LIMINE) $(GENSYMS) $(MKFS) $(FSCK) $(MKFAT)

$(MKFS): tools/mkfs/mkfs.c kernel/include/fs/mfs_format.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCPPFLAGS) -O2 -std=c99 -Wall -Ikernel/include -o $@ $<

$(FSCK): tools/fsck/fsck.c kernel/include/fs/mfs_format.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCPPFLAGS) -O2 -std=c99 -Wall -Ikernel/include -o $@ $<

$(MKFAT): tools/mkfat/mkfat.c kernel/include/fs/fat_format.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCPPFLAGS) -O2 -std=c99 -Wall -Ikernel/include -o $@ $<

$(LIMINE): third_party/limine/limine.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCPPFLAGS) -O2 -std=c99 -o $@ $<

$(GENSYMS): tools/gensyms/gensyms.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCPPFLAGS) -O2 -std=c99 -Wall -o $@ $<

kernel: $(GENSYMS)
	$(MAKE) -C kernel

libc:
	$(MAKE) -C libc

libfont: libc
	$(MAKE) -C libfont

libwire: libc
	$(MAKE) -C libwire

libaudio: libc libwire
	$(MAKE) -C libaudio

libgui: libc libfont libwire
	$(MAKE) -C libgui

user: libc libfont libwire libaudio libgui
	$(MAKE) -C user

# The initrd is a ustar archive of build/initrd_root, populated by user/.
initrd: user
	cd $(BUILD)/initrd_root && tar --format ustar --exclude .DS_Store --exclude ./usr/share/sounds -cf $(INITRD) .

# The disk image is attached as a virtio-blk device and holds the root
# filesystem: an mfs image built from build/initrd_root (M13). It is
# rebuilt whenever a user program changes, which discards files written
# during earlier runs.
$(DISK): $(MKFS) user
	@mkdir -p $(dir $@)
	$(MKFS) $@ $(DISK_MB) $(BUILD)/initrd_root

# Swap lives on a second virtio-blk device (M14), zero filled.
$(SWAP):
	@mkdir -p $(dir $@)
	dd if=/dev/zero of=$@ bs=1048576 count=$(SWAP_MB) status=none

disk: $(DISK) $(SWAP)

# QEMU is started by tools/run.sh, which reads qemu.conf, QEMU_* variables
# and RUNFLAGS. Variables given on the make command line are exported so
# that `make QEMU_AUDIO=none run` keeps working. `run` and `gdb` let the
# script build the image, since the framebuffer mode it picks (VIDEO, or
# QEMU_VIDEO, or a doubled mode on a Retina display) is baked into it.
RUN := tools/run.sh
$(foreach v,QEMU QEMU_AUDIO QEMU_AUDIO_OPTS QEMU_WAV QEMU_SOUND QEMU_MEM QEMU_SMP \
            QEMU_ACCEL QEMU_DISPLAY QEMU_FULLSCREEN QEMU_VIDEO QEMU_SERIAL QEMU_EXTRA \
            QEMU_CONF CMDLINE, \
    $(if $(filter command line,$(origin $(v))),$(eval export $(v))))
ifeq ($(origin VIDEO),command line)
export QEMU_VIDEO := $(VIDEO)
endif

# VIDEO=WxH[xBPP][@SCALE] selects the framebuffer mode (video= on the
# kernel command line); @2 doubles every pixel for high density displays.
image: kernel initrd $(LIMINE) $(DISK) $(SWAP)
	LIMINE=$(LIMINE) INITRD=$(INITRD) tools/mkiso.sh $(KERNEL) $(ISO) "$(strip $(CMDLINE) $(if $(VIDEO),video=$(VIDEO)))"

run:
	ISO=$(ISO) DISK=$(DISK) SWAP=$(SWAP) $(RUN) --build $(RUNFLAGS)

gdb:
	ISO=$(ISO) DISK=$(DISK) SWAP=$(SWAP) $(RUN) --build --gdb $(RUNFLAGS)

# CASES="gui gui_wm" runs only those cases; the whole suite takes too
# long to run for every change.
test: kernel initrd $(LIMINE) $(DISK) $(FSCK) $(MKFAT)
	@LIMINE=$(LIMINE) INITRD=$(INITRD) DISK=$(DISK) MKFS=$(MKFS) MKFAT=$(MKFAT) tests/run_all.sh $(KERNEL) $(BUILD)/tests tests/cases $(CASES)

# The cases whose behaviour depends on the processor or the hypervisor,
# run with hardware virtualization. Linux only; needs /dev/kvm.
KVM_CASES := boot cpu exception fork signals smp smp_user vmm sched
test-kvm:
	ACCEL=kvm $(MAKE) test CASES="$(KVM_CASES)"

# Host unit tests of the GUI framework.
check:
	$(MAKE) -C libfont check
	$(MAKE) -C libwire check
	$(MAKE) -C libgui check

.PHONY: check-sh
check-sh:
	@mkdir -p $(BUILD)/sh/host
	$(HOSTCC) $(HOSTCPPFLAGS) -DSH_TEST -D_DEFAULT_SOURCE -std=c17 -O1 -g -Wall -Wextra \
	    -Wno-unused-parameter -include user/sh/tests/host_compat.h -Iuser/sh \
	    -o $(BUILD)/sh/host/test_parser user/sh/*.c user/sh/tests/test_parser.c
	$(BUILD)/sh/host/test_parser

clean:
	rm -rf $(BUILD)
