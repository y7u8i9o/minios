include ../toolchain.mk

OUT    := $(BUILD)/user
ROOT   := $(BUILD)/initrd_root
LIBC   := $(BUILD)/libc/libc.a
LIBGUI := $(BUILD)/libgui/libgui.a
LIBFONT := $(BUILD)/libfont/libfont.a
LIBWIRE := $(BUILD)/libwire/libwire.a
CRT0   := $(BUILD)/libc/crt0.o

# Every directory holding a main program: one binary per directory named
# after it, plus tests/*.c which each become one binary.
PROGS  := init sh hello edit mint term compositor panel desktop
TESTS  := $(patsubst tests/%.c,%,$(wildcard tests/*.c))
UTILS  := $(patsubst coreutils/%.c,%,$(wildcard coreutils/*.c))
APPS   := $(patsubst apps/%.c,%,$(wildcard apps/*.c))
BINS   := $(addprefix $(ROOT)/bin/,$(PROGS) $(TESTS) $(UTILS) $(APPS))

UCPP   := -I../libc/include -I../kernel/include -I../libgui/include -I../libfont/include -I../libwire/include -I../libwire/generated

ETC    := $(patsubst etc/%,$(ROOT)/etc/%,$(wildcard etc/* etc/*/*))
SHARE  := $(patsubst share/%,$(ROOT)/usr/share/%,$(wildcard share/*/*))
HOME   := $(patsubst home/%,$(ROOT)/home/%,$(wildcard home/*/*))
FONTS  := $(ROOT)/etc/fonts/DejaVuSans.ttf $(ROOT)/etc/fonts/DejaVuSansMono.ttf $(ROOT)/etc/fonts/lmroman10-regular.otf

.PHONY: all clean
all: $(BINS) $(ETC) $(SHARE) $(HOME) $(FONTS) $(ROOT)/dev $(ROOT)/initrd

$(ROOT)/etc/%: etc/%
	@mkdir -p $(dir $@)
	@if [ -f $< ]; then cp $< $@; else mkdir -p $@; fi

$(ROOT)/usr/share/%: share/%
	@mkdir -p $(dir $@)
	cp $< $@

$(ROOT)/home/%: home/%
	@mkdir -p $(dir $@)
	cp $< $@

$(ROOT)/etc/fonts/%.ttf: ../third_party/dejavu/%.ttf
	@mkdir -p $(dir $@)
	cp $< $@

$(ROOT)/etc/fonts/%.otf: ../third_party/lmodern/%.otf
	@mkdir -p $(dir $@)
	cp $< $@

$(ROOT)/dev $(ROOT)/initrd:
	mkdir -p $@

$(ROOT)/bin/%: %/*.c $(LIBC) $(LIBGUI) $(LIBWIRE) $(LIBFONT) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/$*.elf $(ULDFLAGS) $(CRT0) $(filter %.c,$^) $(LIBGUI) $(LIBWIRE) $(LIBFONT) $(LIBC) -lgcc
	cp $(OUT)/$*.elf $@

$(ROOT)/bin/%: coreutils/%.c $(LIBC) $(LIBGUI) $(LIBWIRE) $(LIBFONT) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/$*.elf $(ULDFLAGS) $(CRT0) $< $(LIBGUI) $(LIBWIRE) $(LIBFONT) $(LIBC) -lgcc
	cp $(OUT)/$*.elf $@

$(ROOT)/bin/%: apps/%.c $(LIBC) $(LIBGUI) $(LIBWIRE) $(LIBFONT) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/$*.elf $(ULDFLAGS) $(CRT0) $< $(LIBGUI) $(LIBWIRE) $(LIBFONT) $(LIBC) -lgcc
	cp $(OUT)/$*.elf $@

$(ROOT)/bin/%: tests/%.c $(LIBC) $(LIBGUI) $(LIBWIRE) $(LIBFONT) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/$*.elf $(ULDFLAGS) $(CRT0) $< $(LIBGUI) $(LIBWIRE) $(LIBFONT) $(LIBC) -lgcc
	cp $(OUT)/$*.elf $@

clean:
	rm -rf $(OUT) $(ROOT)
