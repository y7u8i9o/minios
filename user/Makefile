include ../toolchain.mk

OUT    := $(BUILD)/user
ROOT   := $(BUILD)/initrd_root
LIBC   := $(BUILD)/libc/libc.a
LIBGUI := $(BUILD)/libgui/libgui.a
LIBFONT := $(BUILD)/libfont/libfont.a
LIBWIRE := $(BUILD)/libwire/libwire.a
LIBAUDIO := $(BUILD)/libaudio/libaudio.a
LIBEDIT := $(BUILD)/libedit/libedit.a
CRT0   := $(BUILD)/libc/crt0.o

# Every directory holding a main program: one binary per directory named
# after it, plus tests/*.c which each become one binary.
PROGS  := init sh hello edit mint term x12 panel desktop calc audiod settings files
TESTS  := $(patsubst tests/%.c,%,$(wildcard tests/*.c))
UTILS  := $(patsubst coreutils/%.c,%,$(wildcard coreutils/*.c))
APPS   := $(filter-out compsettings,$(patsubst apps/%.c,%,$(wildcard apps/*.c))) x12settings
BINS   := $(addprefix $(ROOT)/bin/,$(PROGS) $(TESTS) $(UTILS) $(APPS) lua luac)

# Lua is compiled unmodified from third_party/lua/src; platform choices
# are made here instead of in luaconf.h (see docs/design/lua.md).
# LUA_USE_POSIX selects _setjmp, getc_unlocked, fseeko, mkstemp, popen
# and the wait status macros, all of which libc provides. Modules load
# only from the share tree and the current directory; there is no
# dynamic loading. lua/ holds the replacement linit.c and the minios
# modules (fs, sys), compiled with the interpreter headers.
LUA_SRC  := ../third_party/lua/src
LUA_CORE := $(filter-out $(LUA_SRC)/lua.c $(LUA_SRC)/luac.c $(LUA_SRC)/linit.c,$(wildcard $(LUA_SRC)/*.c))
LUA_OBJS := $(patsubst $(LUA_SRC)/%.c,$(OUT)/lua/%.o,$(LUA_CORE)) \
            $(patsubst lua/%.c,$(OUT)/lua/minios_%.o,$(wildcard lua/*.c))
LUA_LIBS := $(LIBGUI) $(LIBAUDIO) $(LIBWIRE) $(LIBFONT) $(LIBEDIT)
LUA_CPP  := -DLUA_USE_POSIX -DLUA_PATH_DEFAULT='"/usr/share/lua/5.5/?.lua;/usr/share/lua/5.5/?/init.lua;./?.lua;./?/init.lua"' \
            -DLUA_CPATH_DEFAULT='""'

UCPP   := -I../libc/include -I../kernel/include -I../libgui/include -I../libfont/include -I../libwire/include -I../libwire/generated -I../libaudio/include
UCPP   += -I../libedit/include

ETC    := $(patsubst etc/%,$(ROOT)/etc/%,$(wildcard etc/* etc/*/*))
HOME_FILES := $(patsubst home/%,$(ROOT)/home/%,$(wildcard home/*/*)) $(ROOT)/home/.shrc
FONTS  := $(ROOT)/etc/fonts/DejaVuSans.ttf $(ROOT)/etc/fonts/DejaVuSansMono.ttf $(ROOT)/etc/fonts/lmroman10-regular.otf $(ROOT)/etc/fonts/NotoSans-Regular.ttf $(ROOT)/etc/fonts/unifont.otf

.PHONY: all clean
all: $(BINS) $(ETC) share-tree $(HOME_FILES) $(FONTS) $(ROOT)/dev $(ROOT)/initrd $(ROOT)/tmp

$(ROOT)/etc/%: etc/%
	@mkdir -p $(dir $@)
	@if [ -f $< ]; then cp $< $@; else mkdir -p $@; fi

# The share tree is copied whole: make cannot name files with spaces, and
# media files may carry them.
.PHONY: share-tree
share-tree:
	@mkdir -p $(ROOT)/usr/share
	@cp -Rp share/. $(ROOT)/usr/share/
	@if [ -d ../third_party/fontawesome/svgs ]; then cp ../third_party/fontawesome/svgs/*.svg $(ROOT)/usr/share/icons/; fi

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

$(ROOT)/dev $(ROOT)/initrd $(ROOT)/tmp:
	mkdir -p $@

$(OUT)/lua/%.o: $(LUA_SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UCPP) $(LUA_CPP) -MMD -MP -c -o $@ $<

$(OUT)/lua/minios_%.o: lua/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UCPP) $(LUA_CPP) -I$(LUA_SRC) -MMD -MP -c -o $@ $<

$(ROOT)/bin/lua: $(LUA_OBJS) $(OUT)/lua/lua.o $(LUA_LIBS) $(LIBC) $(CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -o $(OUT)/lua.elf $(ULDFLAGS) $(CRT0) $(LUA_OBJS) $(OUT)/lua/lua.o $(LUA_LIBS) $(LIBC) -lgcc
	cp $(OUT)/lua.elf $@

$(ROOT)/bin/luac: $(LUA_OBJS) $(OUT)/lua/luac.o $(LUA_LIBS) $(LIBC) $(CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -o $(OUT)/luac.elf $(ULDFLAGS) $(CRT0) $(LUA_OBJS) $(OUT)/lua/luac.o $(LUA_LIBS) $(LIBC) -lgcc
	cp $(OUT)/luac.elf $@

-include $(LUA_OBJS:.o=.d) $(OUT)/lua/lua.d $(OUT)/lua/luac.d

$(ROOT)/bin/%: %/*.c $(LIBC) $(LIBGUI) $(LIBAUDIO) $(LIBWIRE) $(LIBFONT) $(LIBEDIT) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/$*.elf $(ULDFLAGS) $(CRT0) $(filter %.c,$^) $(LIBGUI) $(LIBAUDIO) $(LIBWIRE) $(LIBFONT) $(LIBEDIT) $(LIBC) -lgcc
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
