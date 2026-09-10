include ../toolchain.mk

OUT      := $(BUILD)/kernel
KERNEL   ?= $(BUILD)/kernel.elf
GENSYMS  ?= $(BUILD)/host/gensyms

SUBDIRS  := arch/x86_64 lib mm sync sched syscall fs fs/mfs fs/fat ipc net block audio input drivers drivers/virtio debug
ifeq ($(strip $(CONFIG_TESTS)),1)
SUBDIRS  += tests
endif

CSRC     := $(foreach d,$(SUBDIRS),$(wildcard $(d)/*.c))
SSRC     := $(foreach d,$(SUBDIRS),$(wildcard $(d)/*.S))
OBJS     := $(patsubst %.c,$(OUT)/%.o,$(CSRC)) $(patsubst %.S,$(OUT)/%.o,$(SSRC))
DEPS     := $(OBJS:.o=.d)

CPPFLAGS := -Iinclude -I../third_party/limine

.PHONY: all clean
all: $(KERNEL)

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

$(OUT)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(KASFLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

# Pass 1: link with an empty symbol table. The .ksyms section is filled by a
# generated assembly stub that .incbin's the blob.
$(OUT)/ksyms_empty.bin:
	@mkdir -p $(dir $@)
	: > $@

$(OUT)/ksyms_empty.o: $(OUT)/ksyms_empty.bin
	printf '.section .ksyms,"a"\n.incbin "%s"\n' $< > $(OUT)/ksyms_empty.S
	$(CC) $(KASFLAGS) -c -o $@ $(OUT)/ksyms_empty.S

# The identification (VERSION, the build number, the commit) is generated
# before pass 1 so that both passes link the same objects.
$(OUT)/kernel_pass1.elf: linker.ld $(OBJS) $(OUT)/ksyms_empty.o ../tools/version.sh ../VERSION
	../tools/version.sh .. $(OUT)/version.c
	$(CC) $(KCFLAGS) $(CPPFLAGS) -c -o $(OUT)/version.o $(OUT)/version.c
	$(LD) $(KLDFLAGS) -T linker.ld -o $@ $(OBJS) $(OUT)/version.o $(OUT)/ksyms_empty.o

# Pass 2: extract symbols and relink with the populated .ksyms section.
# .ksyms is the last section in the image, so no other address changes.
$(OUT)/ksyms.bin: $(OUT)/kernel_pass1.elf $(GENSYMS)
	$(NM) -n -S $< | $(GENSYMS) $@

$(OUT)/ksyms.o: $(OUT)/ksyms.bin
	printf '.section .ksyms,"a"\n.incbin "%s"\n' $< > $(OUT)/ksyms.S
	$(CC) $(KASFLAGS) -c -o $@ $(OUT)/ksyms.S

$(KERNEL): linker.ld $(OBJS) $(OUT)/ksyms.o
	$(LD) $(KLDFLAGS) -T linker.ld -o $@ $(OBJS) $(OUT)/version.o $(OUT)/ksyms.o

clean:
	rm -rf $(OUT) $(KERNEL)

-include $(DEPS)
