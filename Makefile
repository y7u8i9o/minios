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

export TOP BUILD KERNEL LIMINE GENSYMS INITRD DISK MKFS SWAP

.PHONY: all kernel libc libfont libwire libgui user initrd disk image run gdb test check clean tools $(DISK)

all: kernel libc user

tools: $(LIMINE) $(GENSYMS) $(MKFS)

$(MKFS): tools/mkfs/mkfs.c kernel/include/fs/mfs_format.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -std=c99 -Wall -Ikernel/include -o $@ $<

$(LIMINE): third_party/limine/limine.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -std=c99 -o $@ $<

$(GENSYMS): tools/gensyms/gensyms.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -std=c99 -Wall -o $@ $<

kernel: $(GENSYMS)
	$(MAKE) -C kernel

libc:
	$(MAKE) -C libc

libfont: libc
	$(MAKE) -C libfont

libwire: libc
	$(MAKE) -C libwire

libgui: libc libfont libwire
	$(MAKE) -C libgui

user: libc libfont libwire libgui
	$(MAKE) -C user

# The initrd is a ustar archive of build/initrd_root, populated by user/.
initrd: user
	cd $(BUILD)/initrd_root && tar --format ustar --exclude .DS_Store -cf $(INITRD) .

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

QEMU_DISK := -drive file=$(DISK),if=none,id=vd0,format=raw -device virtio-blk-pci,drive=vd0 \
             -drive file=$(SWAP),if=none,id=vd1,format=raw -device virtio-blk-pci,drive=vd1

image: kernel initrd $(LIMINE) $(DISK) $(SWAP)
	LIMINE=$(LIMINE) INITRD=$(INITRD) tools/mkiso.sh $(KERNEL) $(ISO) "$(CMDLINE)"

run: image
	$(QEMU) $(QEMU_FLAGS) $(QEMU_DISK) -cdrom $(ISO)

gdb: image
	@echo "Connect with: $(GDB) -iex 'set auto-load safe-path $(TOP)'"
	$(QEMU) $(QEMU_FLAGS) $(QEMU_DISK) -cdrom $(ISO) -s -S

test: kernel initrd $(LIMINE) $(DISK)
	@LIMINE=$(LIMINE) INITRD=$(INITRD) DISK=$(DISK) tests/run_all.sh $(KERNEL) $(BUILD)/tests tests/cases

# Host unit tests of the GUI framework.
check:
	$(MAKE) -C libwire check
	$(MAKE) -C libgui check

clean:
	rm -rf $(BUILD)
