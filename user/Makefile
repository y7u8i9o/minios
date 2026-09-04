include ../toolchain.mk

OUT    := $(BUILD)/user
ROOT   := $(BUILD)/initrd_root
LIBC   := $(BUILD)/libc/libc.a
LIBGUI := $(BUILD)/libgui/libgui.a
LIBFONT := $(BUILD)/libfont/libfont.a
LIBWIRE := $(BUILD)/libwire/libwire.a
LIBAUDIO := $(BUILD)/libaudio/libaudio.a
CRT0   := $(BUILD)/libc/crt0.o

# Every directory holding a main program: one binary per directory named
# after it, plus tests/*.c which each become one binary.
PROGS  := init sh hello edit mint term x12 panel desktop calc audiod settings
TESTS  := $(patsubst tests/%.c,%,$(wildcard tests/*.c))
UTILS  := $(patsubst coreutils/%.c,%,$(wildcard coreutils/*.c))
APPS   := $(filter-out compsettings,$(patsubst apps/%.c,%,$(wildcard apps/*.c))) x12settings
BINS   := $(addprefix $(ROOT)/bin/,$(PROGS) $(TESTS) $(UTILS) $(APPS))

UCPP   := -I../libc/include -I../kernel/include -I../libgui/include -I../libfont/include -I../libwire/include -I../libwire/generated -I../libaudio/include

ETC    := $(patsubst etc/%,$(ROOT)/etc/%,$(wildcard etc/* etc/*/*))
HOME   := $(patsubst home/%,$(ROOT)/home/%,$(wildcard home/*/*))
FONTS  := $(ROOT)/etc/fonts/DejaVuSans.ttf $(ROOT)/etc/fonts/DejaVuSansMono.ttf $(ROOT)/etc/fonts/lmroman10-regular.otf $(ROOT)/etc/fonts/NotoSans-Regular.ttf $(ROOT)/etc/fonts/unifont.otf

.PHONY: all clean
all: $(BINS) $(ETC) share-tree $(HOME) $(FONTS) $(ROOT)/dev $(ROOT)/initrd

$(ROOT)/etc/%: etc/%
	@mkdir -p $(dir $@)
	@if [ -f $< ]; then cp $< $@; else mkdir -p $@; fi

# The share tree is copied whole: make cannot name files with spaces, and
# media files may carry them.
.PHONY: share-tree
share-tree:
	@mkdir -p $(ROOT)/usr/share
	@cp -Rp share/. $(ROOT)/usr/share/

$(ROOT)/home/%: home/%
	@mkdir -p $(dir $@)
	cp $< $@

$(ROOT)/etc/fonts/%.ttf: ../third_party/dejavu/%.ttf
	@mkdir -p $(dir $@)
	cp $< $@

$(ROOT)/etc/fonts/%.otf: ../third_party/lmodern/%.otf
	@mkdir -p $(dir $@)
	cp $< $@

$(ROOT)/etc/fonts/NotoSans-Regular.ttf: ../third_party/notosans/NotoSans-Regular.ttf
	@mkdir -p $(dir $@)
	cp $< $@

# Installed without its release number so that upgrading Unifont does not
# touch the paths compiled into the Unicode viewer; the release is
# recorded in third_party/unifont/NOTICE.
$(ROOT)/etc/fonts/unifont.otf: ../third_party/unifont/unifont-17.0.05.otf
	@mkdir -p $(dir $@)
	cp $< $@

$(ROOT)/dev $(ROOT)/initrd:
	mkdir -p $@

$(ROOT)/bin/%: %/*.c $(LIBC) $(LIBGUI) $(LIBAUDIO) $(LIBWIRE) $(LIBFONT) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/$*.elf $(ULDFLAGS) $(CRT0) $(filter %.c,$^) $(LIBGUI) $(LIBAUDIO) $(LIBWIRE) $(LIBFONT) $(LIBC) -lgcc
	cp $(OUT)/$*.elf $@

# X12 keeps its implementation in compositor/ for source-history
# continuity; the installed server and user-facing name are X12.
$(ROOT)/bin/x12: compositor/*.c $(LIBC) $(LIBGUI) $(LIBAUDIO) $(LIBWIRE) $(LIBFONT) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/x12.elf $(ULDFLAGS) $(CRT0) $(filter %.c,$^) $(LIBGUI) $(LIBAUDIO) $(LIBWIRE) $(LIBFONT) $(LIBC) -lgcc
	cp $(OUT)/x12.elf $@

$(ROOT)/bin/x12settings: apps/compsettings.c $(LIBC) $(LIBGUI) $(LIBAUDIO) $(LIBWIRE) $(LIBFONT) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/x12settings.elf $(ULDFLAGS) $(CRT0) $< $(LIBGUI) $(LIBAUDIO) $(LIBWIRE) $(LIBFONT) $(LIBC) -lgcc
	cp $(OUT)/x12settings.elf $@

# Named explicitly so that the generated block table is a prerequisite;
# the generic apps rule below tracks only the .c file.
$(ROOT)/bin/unicode: apps/unicode.c apps/unicode_blocks.h $(LIBC) $(LIBGUI) $(LIBAUDIO) $(LIBWIRE) $(LIBFONT) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/unicode.elf $(ULDFLAGS) $(CRT0) $< $(LIBGUI) $(LIBAUDIO) $(LIBWIRE) $(LIBFONT) $(LIBC) -lgcc
	cp $(OUT)/unicode.elf $@

$(ROOT)/bin/%: coreutils/%.c $(LIBC) $(LIBGUI) $(LIBAUDIO) $(LIBWIRE) $(LIBFONT) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/$*.elf $(ULDFLAGS) $(CRT0) $< $(LIBGUI) $(LIBAUDIO) $(LIBWIRE) $(LIBFONT) $(LIBC) -lgcc
	cp $(OUT)/$*.elf $@

$(ROOT)/bin/%: apps/%.c $(LIBC) $(LIBGUI) $(LIBAUDIO) $(LIBWIRE) $(LIBFONT) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/$*.elf $(ULDFLAGS) $(CRT0) $< $(LIBGUI) $(LIBAUDIO) $(LIBWIRE) $(LIBFONT) $(LIBC) -lgcc
	cp $(OUT)/$*.elf $@

$(ROOT)/bin/%: tests/%.c $(LIBC) $(LIBGUI) $(LIBAUDIO) $(LIBWIRE) $(LIBFONT) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/$*.elf $(ULDFLAGS) $(CRT0) $< $(LIBGUI) $(LIBAUDIO) $(LIBWIRE) $(LIBFONT) $(LIBC) -lgcc
	cp $(OUT)/$*.elf $@

clean:
	rm -rf $(OUT) $(ROOT)
