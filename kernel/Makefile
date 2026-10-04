include ../toolchain.mk

OUT      := $(BUILD)/kernel
KERNEL   ?= $(BUILD)/kernel.elf
GENSYMS  ?= $(BUILD)/host/gensyms

SUBDIRS  := arch/$(ARCH) init lib mm sync sched syscall fs fs/mfs fs/fat ipc net net/tcp block audio input drivers drivers/virtio drivers/usb debug
ifeq ($(strip $(CONFIG_TESTS)),1)
SUBDIRS  += tests
endif

# Self-tests of x86 processor features, built only for x86_64.
TESTS_X86_ONLY := tests/test_cpu.c
CSRC     := $(foreach d,$(SUBDIRS),$(wildcard $(d)/*.c))
ifneq ($(ARCH),x86_64)
CSRC     := $(filter-out $(TESTS_X86_ONLY),$(CSRC))
endif
SSRC     := $(foreach d,$(SUBDIRS),$(wildcard $(d)/*.S))
OBJS     := $(patsubst %.c,$(OUT)/%.o,$(CSRC)) $(patsubst %.S,$(OUT)/%.o,$(SSRC))
DEPS     := $(OBJS:.o=.d)

CPPFLAGS := -Iinclude -Iarch/$(ARCH)/include -I../third_party/limine
LDSCRIPT := arch/$(ARCH)/linker.ld

# uACPI reads the ACPI tables of an aarch64 machine that has no device tree
# (docs/design/acpi.md). It is built in the barebones mode, which contains
# the table interface only. Its sources are third party code. They are
# compiled without -Werror, so that a warning of a newer compiler in them
# does not stop the build.
ifeq ($(ARCH),aarch64)
UACPI_DIR  := ../third_party/uacpi
UACPI_OBJS := $(patsubst $(UACPI_DIR)/source/%.c,$(OUT)/uacpi/%.o,$(wildcard $(UACPI_DIR)/source/*.c))
CPPFLAGS   += -DUACPI_BAREBONES_MODE -I$(UACPI_DIR)/include
OBJS       += $(UACPI_OBJS)
DEPS       += $(UACPI_OBJS:.o=.d)
endif

.PHONY: all clean
all: $(KERNEL)

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

$(OUT)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(KASFLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

$(OUT)/uacpi/%.o: $(UACPI_DIR)/source/%.c
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -Werror -Wmissing-prototypes,$(KCFLAGS)) $(CPPFLAGS) -MMD -MP -c -o $@ $<

# The first pass links with an empty symbol table. The .ksyms section is
# filled by a generated assembly stub that includes the blob with .incbin.
$(OUT)/ksyms_empty.bin:
	@mkdir -p $(dir $@)
	: > $@

$(OUT)/ksyms_empty.o: $(OUT)/ksyms_empty.bin
	printf '.section .ksyms,"a"\n.incbin "%s"\n' $< > $(OUT)/ksyms_empty.S
	$(CC) $(KASFLAGS) -c -o $@ $(OUT)/ksyms_empty.S

# The identification (VERSION, the build number, the commit) is generated
# before the first pass, and both passes therefore link the same objects.
$(OUT)/kernel_pass1.elf: $(LDSCRIPT) $(OBJS) $(OUT)/ksyms_empty.o ../tools/version.sh ../VERSION
	../tools/version.sh .. $(OUT)/version.c
	$(CC) $(KCFLAGS) $(CPPFLAGS) -c -o $(OUT)/version.o $(OUT)/version.c
	$(LD) $(KLDFLAGS) -T $(LDSCRIPT) -o $@ $(OBJS) $(OUT)/version.o $(OUT)/ksyms_empty.o

# The second pass extracts the symbols and relinks with the populated
# .ksyms section. Because .ksyms is the last section in the image, no other
# address changes.
$(OUT)/ksyms.bin: $(OUT)/kernel_pass1.elf $(GENSYMS)
	$(NM) -n -S $< | $(GENSYMS) $@

$(OUT)/ksyms.o: $(OUT)/ksyms.bin
	printf '.section .ksyms,"a"\n.incbin "%s"\n' $< > $(OUT)/ksyms.S
	$(CC) $(KASFLAGS) -c -o $@ $(OUT)/ksyms.S

$(KERNEL): $(LDSCRIPT) $(OBJS) $(OUT)/ksyms.o
	$(LD) $(KLDFLAGS) -T $(LDSCRIPT) -o $@ $(OBJS) $(OUT)/version.o $(OUT)/ksyms.o

clean:
	rm -rf $(OUT) $(KERNEL)

-include $(DEPS)
