include ../toolchain.mk

OUT    := $(BUILD)/user
ROOT   := $(BUILD)/initrd_root
LIBC   := $(BUILD)/libc/libc.a
LIBEDIT := $(BUILD)/libedit/libedit.a
CRT0   := $(BUILD)/libc/crt0.o
# The shared libraries (docs/design/dynlink.md). Programs name them with
# -l and --as-needed keeps only the ones a program references; libedit is
# static, since a single program uses it.
SHARED := libc libfont libwire libaudio libgui liblua
SOS    := $(addprefix $(BUILD)/lib/,$(addsuffix .so,$(SHARED)))
DYNLIBS := -lgui -laudio -lwire -lfont $(LIBEDIT) -lc
# Every program is installed without its debugging sections; the symbol
# table stays for the profiler.
INSTALL = $(OBJCOPY) --strip-debug $(OUT)/$*.elf $@

# Every directory holding a main program: one binary per directory named
# after it, plus tests/*.c which each become one binary.
PROGS  := init sh hello edit mint term x12 panel desktop audiod settings files pkg
TESTS  := $(patsubst tests/%.c,%,$(wildcard tests/*.c))
UTILS  := $(filter-out code,$(patsubst coreutils/%.c,%,$(wildcard coreutils/*.c)))
APPS   := clock evtest logview profiler sysmon wireview x12settings
APPBIN := $(OUT)/app-bin
BINS   := $(addprefix $(ROOT)/bin/,$(PROGS) $(TESTS) $(UTILS) $(APPS) lua luac sed awk make tar tcc)
LIBS   := $(addprefix $(ROOT)/lib/,ld.so abi $(addsuffix .so,$(SHARED)))

# Lua is compiled unmodified from third_party/lua/src; platform choices
# are made here instead of in luaconf.h (see docs/design/lua.md).
# LUA_USE_POSIX selects _setjmp, getc_unlocked, fseeko, mkstemp, popen
# and the wait status macros, all of which libc provides. Modules load
# only from the share tree and the current directory; there is no
# dynamic loading. lua/ holds the replacement linit.c and the minios
# modules (fs, sys), compiled with the interpreter headers.
LUA_SRC  := ../third_party/lua/src
LUA_CORE := $(filter-out $(LUA_SRC)/lua.c $(LUA_SRC)/luac.c $(LUA_SRC)/linit.c,$(wildcard $(LUA_SRC)/*.c))
LUA_CORE_OBJS := $(patsubst $(LUA_SRC)/%.c,$(OUT)/lua/%.o,$(LUA_CORE))
LUA_OBJS := $(patsubst lua/%.c,$(OUT)/lua/minios_%.o,$(wildcard lua/*.c))
LUA_CPP  := -DLUA_USE_POSIX -DLUA_PATH_DEFAULT='"/usr/share/lua/5.5/?.lua;/usr/share/lua/5.5/?/init.lua;./?.lua;./?/init.lua"' \
            -DLUA_CPATH_DEFAULT='""'

UCPP   := -I../libc/include -I../kernel/include -I../libgui/include -I../libfont/include -I../libwire/include -I../libwire/generated -I../libaudio/include
UCPP   += -I../libedit/include

ETC    := $(patsubst etc/%,$(ROOT)/etc/%,$(wildcard etc/* etc/*/*))
HOME_FILES := $(patsubst home/%,$(ROOT)/home/%,$(wildcard home/*/*)) $(ROOT)/home/.shrc
FONTS  := $(ROOT)/etc/fonts/DejaVuSans.ttf $(ROOT)/etc/fonts/DejaVuSansMono.ttf $(ROOT)/etc/fonts/lmroman10-regular.otf $(ROOT)/etc/fonts/NotoSans-Regular.ttf $(ROOT)/etc/fonts/unifont.otf

.PHONY: all clean
all: $(LIBS) $(BINS) $(ETC) share-tree skel-tree $(HOME_FILES) $(FONTS) $(ROOT)/dev $(ROOT)/initrd $(ROOT)/tmp tcc-support links

$(ROOT)/etc/%: etc/%
	@mkdir -p $(dir $@)
	@if [ -f $< ]; then cp $< $@; else mkdir -p $@; fi

# Two symbolic links that the symlink test resolves on the root image and
# under /initrd, which mkfs and the initrd reader store as links: a
# relative one to the tar fixture beside it and an absolute one to
# /etc/motd. They are recreated on every build.
.PHONY: links
links: $(ETC)
	@ln -sfn fixture.tar $(ROOT)/etc/tests/link-fixture.tar
	@ln -sfn /etc/motd $(ROOT)/etc/tests/link-motd

# pkg-key installs the public half of the key that signs the package
# repository index (docs/design/packages.md), which the top level Makefile
# derives. The copy compares contents, since a change of PKG_KEY can leave
# timestamps equal to the second.
.PHONY: pkg-key
all: pkg-key
pkg-key:
	@mkdir -p $(ROOT)/etc/pkg/keys
	@cmp -s $(PKG_PUB) $(ROOT)/etc/pkg/keys/build.pub || cp $(PKG_PUB) $(ROOT)/etc/pkg/keys/build.pub

# The share tree is copied whole: make cannot name files with spaces, and
# media files may carry them.
.PHONY: share-tree
share-tree:
	@mkdir -p $(ROOT)/usr/share/lua/5.5
	@cp -Rp share/. $(ROOT)/usr/share/
	@if [ -d ../third_party/fontawesome/svgs ]; then cp ../third_party/fontawesome/svgs/*.svg $(ROOT)/usr/share/icons/; fi

# The home directory is installed twice: on the root image, for a boot
# without a data volume, and as the skeleton that fsinit copies onto a
# fresh data volume (etc/fstab).
.PHONY: skel-tree
skel-tree:
	@mkdir -p $(ROOT)/usr/share/skel/home
	@cp -Rp home/. $(ROOT)/usr/share/skel/home/

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
	$(CC) $(UCFLAGS) $(UCPP) $(LUA_CPP) $(LUA_RL) -MMD -MP -c -o $@ $<

# The interactive prompt edits lines with libedit. The header defines the
# readline hooks of lua.c, and lua/lreadline.c implements them.
$(OUT)/lua/lua.o: LUA_RL := -include lua/lreadline.h
$(OUT)/lua/lua.o: lua/lreadline.h

$(OUT)/lua/minios_%.o: lua/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UCPP) $(LUA_CPP) -I$(LUA_SRC) -MMD -MP -c -o $@ $<

# The Lua core is one shared object used by lua and luac.
# The ABI number of the Lua core (docs/design/packages.md).
LUA_ABI := 1
$(BUILD)/lib/liblua.abi: Makefile
	@mkdir -p $(dir $@)
	@echo $(LUA_ABI) > $@

# The ABI table the package installer reads: one "soname number" line per
# shared library, from the ABI variables of the library Makefiles.
$(ROOT)/lib/abi: $(addprefix $(BUILD)/lib/,$(addsuffix .abi,$(SHARED)))
	@mkdir -p $(dir $@)
	@rm -f $@
	@for l in $(SHARED); do echo "$$l.so $$(cat $(BUILD)/lib/$$l.abi)" >> $@; done

$(BUILD)/lib/liblua.so: $(LUA_CORE_OBJS) $(BUILD)/lib/libc.so
	@mkdir -p $(dir $@)
	$(LD) $(USOFLAGS) -soname liblua.so -o $@ $(LUA_CORE_OBJS) -L$(BUILD)/lib -lc $(LIBGCC)

$(ROOT)/bin/lua: $(LUA_OBJS) $(OUT)/lua/lua.o $(SOS) $(LIBEDIT) $(CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -o $(OUT)/lua.elf $(ULDFLAGS) $(CRT0) $(LUA_OBJS) $(OUT)/lua/lua.o -llua $(DYNLIBS) -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/lua.elf $@

# luac uses internal functions of the core, which the shared object hides
# (LUAI_FUNC in luaconf.h), so it links the core objects directly.
$(ROOT)/bin/luac: $(LUA_OBJS) $(LUA_CORE_OBJS) $(OUT)/lua/luac.o $(SOS) $(LIBEDIT) $(CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -o $(OUT)/luac.elf $(ULDFLAGS) $(CRT0) $(LUA_OBJS) $(LUA_CORE_OBJS) $(OUT)/lua/luac.o $(DYNLIBS) -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/luac.elf $@

-include $(LUA_CORE_OBJS:.o=.d) $(LUA_OBJS:.o=.d) $(OUT)/lua/lua.d $(OUT)/lua/luac.d

# sed and awk are compiled unmodified from third_party/ (see
# docs/design/sedawk.md). The awk grammar is generated with yacc and the
# procedure table with maketab, a host program of the awk sources.
SED_SRC  := ../third_party/sed/src
SED_OBJS := $(patsubst $(SED_SRC)/%.c,$(OUT)/sed/%.o,$(wildcard $(SED_SRC)/*.c))
AWK_SRC  := ../third_party/awk/src
AWK_GEN  := $(OUT)/awk/gen
AWK_CORE := b.c lex.c lib.c main.c parse.c run.c tran.c
AWK_OBJS := $(patsubst %.c,$(OUT)/awk/%.o,$(AWK_CORE)) $(OUT)/awk/awkgram.tab.o $(OUT)/awk/proctab.o
PORT_CFLAGS := -Wno-unused-but-set-variable -Wno-sign-compare -Wno-implicit-fallthrough -Wno-missing-field-initializers

$(OUT)/sed/%.o: $(SED_SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(PORT_CFLAGS) $(UCPP) -I$(SED_SRC) -MMD -MP -c -o $@ $<

$(ROOT)/bin/sed: $(SED_OBJS) $(BUILD)/lib/libc.so $(CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -o $(OUT)/sed.elf $(ULDFLAGS) $(CRT0) $(SED_OBJS) -lc -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/sed.elf $@

$(AWK_GEN)/awkgram.tab.c $(AWK_GEN)/awkgram.tab.h: $(AWK_SRC)/awkgram.y
	@mkdir -p $(AWK_GEN)
	cd $(AWK_GEN) && $(YACC) -d -b awkgram $(CURDIR)/$(AWK_SRC)/awkgram.y

$(AWK_GEN)/maketab: $(AWK_SRC)/maketab.c $(AWK_GEN)/awkgram.tab.h
	$(HOSTCC) $(HOSTCPPFLAGS) -O1 -I$(AWK_SRC) -I$(AWK_GEN) -o $@ $<

$(AWK_GEN)/proctab.c: $(AWK_GEN)/maketab
	$(AWK_GEN)/maketab $(AWK_GEN)/awkgram.tab.h > $@

$(OUT)/awk/%.o: $(AWK_SRC)/%.c $(AWK_GEN)/awkgram.tab.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(PORT_CFLAGS) $(UCPP) -I$(AWK_SRC) -I$(AWK_GEN) -MMD -MP -c -o $@ $<

$(OUT)/awk/%.o: $(AWK_GEN)/%.c $(AWK_GEN)/awkgram.tab.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(PORT_CFLAGS) -Wno-unused-variable -Wno-unused-function -Wno-unused-label $(UCPP) -I$(AWK_SRC) -I$(AWK_GEN) -MMD -MP -c -o $@ $<

$(ROOT)/bin/awk: $(AWK_OBJS) $(BUILD)/lib/libc.so $(CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -o $(OUT)/awk.elf $(ULDFLAGS) $(CRT0) $(AWK_OBJS) -lc -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/awk.elf $@

-include $(SED_OBJS:.o=.d) $(AWK_OBJS:.o=.d)

# make is pdpmake, compiled unmodified from third_party/make (see
# docs/design/make.md).
MAKE_SRC  := ../third_party/make/src
MAKE_OBJS := $(patsubst $(MAKE_SRC)/%.c,$(OUT)/make/%.o,$(wildcard $(MAKE_SRC)/*.c))

$(OUT)/make/%.o: $(MAKE_SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(PORT_CFLAGS) $(UCPP) -I$(MAKE_SRC) -MMD -MP -c -o $@ $<

$(ROOT)/bin/make: $(MAKE_OBJS) $(BUILD)/lib/libc.so $(CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -o $(OUT)/make.elf $(ULDFLAGS) $(CRT0) $(MAKE_OBJS) -lc -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/make.elf $@

-include $(MAKE_OBJS:.o=.d)

# tar is the tar of sbase, compiled unmodified from third_party/sbase with
# the utility library files it needs (see docs/design/artar.md). sbase's
# strlcpy is renamed so that it does not collide with the libc one.
TAR_SRC  := ../third_party/sbase/src
TAR_UTIL := eprintf.c ealloc.c reallocarray.c mkdirp.c recurse.c strlcpy.c
TAR_OBJS := $(OUT)/tar/tar.o $(patsubst %.c,$(OUT)/tar/util_%.o,$(TAR_UTIL))

$(OUT)/tar/compat.h:
	@mkdir -p $(dir $@)
	printf '/* sbase expects a compat.h generated by its Makefile; minios needs nothing in it. */\n' > $@

$(OUT)/tar/tar.o: $(TAR_SRC)/tar.c $(OUT)/tar/compat.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(PORT_CFLAGS) -Wno-unterminated-string-initialization -Wno-maybe-uninitialized $(UCPP) -I$(TAR_SRC) -I$(OUT)/tar -MMD -MP -c -o $@ $<

$(OUT)/tar/util_%.o: $(TAR_SRC)/libutil/%.c $(OUT)/tar/compat.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(PORT_CFLAGS) $(UCPP) -I$(TAR_SRC) -I$(OUT)/tar -Dstrlcpy=sbase_strlcpy -MMD -MP -c -o $@ $<

$(ROOT)/bin/tar: $(TAR_OBJS) $(BUILD)/lib/libc.so $(CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -o $(OUT)/tar.elf $(ULDFLAGS) $(CRT0) $(TAR_OBJS) -lc -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/tar.elf $@

-include $(TAR_OBJS:.o=.d)

# tcc is the Tiny C Compiler, compiled unmodified from the submodule
# third_party/tinycc as one source with the configuration on the command
# line (docs/design/tcc.md). The runtime library libtcc1.a is built from
# lib/ with the cross compiler. tcc-support installs the system headers,
# the C runtime objects and the compiler's own headers that tcc needs to
# compile programs on minios.
TCC_SRC  := ../third_party/tinycc
TCC_GEN  := $(OUT)/tcc
TCC_VERSION := $(shell cat $(TCC_SRC)/VERSION)
TCC_DEFS := -DONE_SOURCE=1 -DTCC_TARGET_$(TCC_TARGET) -DCONFIG_TCC_SEMLOCK=0 -DCONFIG_TCC_BCHECK=0 \
            -DCONFIG_TCC_BACKTRACE=0 -DTCC_VERSION='"$(TCC_VERSION)"' -DCONFIG_TCCDIR='"/usr/lib/tcc"' \
            -DCONFIG_TCC_ELFINTERP='"/lib/ld.so"' -DCONFIG_TCC_SYSINCLUDEPATHS='"{B}/include:/usr/include"' \
            -DCONFIG_TCC_LIBPATHS='"{B}:/lib"' -DCONFIG_TCC_CRTPREFIX='"/lib"'
TCC_LIB1 := builtin.c stdatomic.c dsohandle.c alloca.S alloca-bt.S atomic.S
ifeq ($(TCC_TARGET),ARM64)
# The binary128 long double arithmetic and __clear_cache for tcc -run.
TCC_LIB1 += lib-arm64.c armflush.c
else
TCC_LIB1 += libtcc1.c va_list.c
endif
TCC_LIB1_OBJS := $(addprefix $(TCC_GEN)/lib/,$(addsuffix .o,$(basename $(TCC_LIB1))))
TCC_TESTS := $(TCC_SRC)/tests/tests2

$(TCC_GEN)/config.h:
	@mkdir -p $(dir $@)
	printf '/* The configuration of tcc is given on the compiler command line. */\n' > $@

$(TCC_GEN)/tcc.o: $(TCC_SRC)/tcc.c $(TCC_GEN)/config.h Makefile
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -ffreestanding -fno-stack-protector -fPIC $(UARCHFLAGS) -O2 -g -fno-builtin -w \
	    $(TCC_DEFS) -I../libc/include -I../kernel/include -I$(TCC_GEN) -I$(TCC_SRC) -MMD -MP -c -o $@ $<

$(ROOT)/bin/tcc: $(TCC_GEN)/tcc.o $(BUILD)/lib/libc.so $(CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -o $(OUT)/tcc.elf $(ULDFLAGS) $(CRT0) $< -lc -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/tcc.elf $@

# armflush.c calls the tcc builtin for the cache maintenance. The GCC
# builtin expands to the libgcc routine for aarch64.
$(TCC_GEN)/lib/armflush.o: TCC_LIB1_CFLAGS := -D__arm64_clear_cache=__builtin___clear_cache

$(TCC_GEN)/lib/%.o: $(TCC_SRC)/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -ffreestanding -fno-stack-protector -fPIC -fno-omit-frame-pointer $(UARCHFLAGS) -O2 -w \
	    $(TCC_LIB1_CFLAGS) -DTCC_TARGET_$(TCC_TARGET) -I../libc/include -I../kernel/include -I$(TCC_SRC) -I$(TCC_SRC)/include -c -o $@ $<

$(TCC_GEN)/lib/%.o: $(TCC_SRC)/lib/%.S
	@mkdir -p $(dir $@)
	$(CC) -DTCC_TARGET_$(TCC_TARGET) -I$(TCC_SRC) -c -o $@ $<

$(ROOT)/usr/lib/tcc/libtcc1.a: $(TCC_LIB1_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $(TCC_LIB1_OBJS)

$(ROOT)/usr/lib/tcc/runmain.o: $(TCC_SRC)/lib/runmain.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -ffreestanding -fno-stack-protector -fPIC -O2 -w -DTCC_TARGET_$(TCC_TARGET) -I../libc/include -I../kernel/include -I$(TCC_SRC) -c -o $@ $<

# crt1.o is the C runtime start; crti.o defines the empty _init and _fini
# and crtn.o is empty, the objects tcc links around every program; libm.a
# is empty because the math functions are in libc.so.
$(ROOT)/lib/crt1.o: $(CRT0)
	@mkdir -p $(dir $@)
	cp $< $@

$(ROOT)/lib/crti.o: $(BUILD)/libc/crti.o
	@mkdir -p $(dir $@)
	cp $< $@

$(ROOT)/lib/crtn.o: $(BUILD)/libc/crtn.o
	@mkdir -p $(dir $@)
	cp $< $@

$(ROOT)/lib/libm.a:
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@

# A selection of the tcc test suite runs on minios (tests/cases/tcc): the
# programs that need neither arguments, input files, bound checking,
# threads, thread local storage nor a specific assembler.
TCC_TEST_NAMES := 00_assignment 01_comment 02_printf 03_struct 04_for 05_array 06_case 07_function \
    08_while 09_do_while 10_pointer 11_precedence 12_hashdefine 13_integer_literals 14_if 15_recursion \
    16_nesting 17_enum 18_include 19_pointer_arithmetic 20_pointer_comparison 21_char_array \
    22_floating_point 23_type_coercion 24_math_library 25_quicksort 26_character_constants 27_sizeof \
    28_strings 29_array_address 30_hanoi 33_ternary_op 35_sizeof \
    36_array_initialisers 37_sprintf 38_multiple_array_index 39_typedef 41_hashif 42_function_pointer \
    43_void_param 44_scoped_declarations 45_empty_for 47_switch_return 48_nested_break \
    49_bracket_evaluation 50_logical_second_arg 51_static 52_unnamed_enum 54_goto 55_lshift_type \
    61_integers 64_macro_nesting 67_macro_concat 70_floating_point_literals

.PHONY: tcc-support
# The headers and the static archives of every library go beside the C
# library, so that a program compiled on minios can use the toolkit, the
# protocol library, the audio client, the line editor and the Lua core
# with the same headers and the same objects as the cross build.
STATIC_LIBS := libc libgui libfont libwire libaudio libedit
tcc-support: $(ROOT)/usr/lib/tcc/libtcc1.a $(ROOT)/usr/lib/tcc/runmain.o $(ROOT)/lib/crt1.o $(ROOT)/lib/crti.o $(ROOT)/lib/crtn.o $(ROOT)/lib/libm.a
	@mkdir -p $(ROOT)/usr/include/minios $(ROOT)/usr/include/gui $(ROOT)/usr/include/font $(ROOT)/usr/include/wire \
	    $(ROOT)/usr/include/audio $(ROOT)/usr/include/lua $(ROOT)/usr/lib/tcc/include $(ROOT)/usr/share/tcc/tests2
	@cp -Rp ../libc/include/. $(ROOT)/usr/include/
	@cp -p ../kernel/include/minios/*.h $(ROOT)/usr/include/minios/
	@cp -p ../kernel/include/syscall_nums.h $(ROOT)/usr/include/
	@cp -p ../libgui/include/gui/*.h $(ROOT)/usr/include/gui/
	@cp -p ../libfont/include/font/*.h $(ROOT)/usr/include/font/
	@cp -p ../libwire/include/wire/*.h $(ROOT)/usr/include/wire/
	@cp -p ../libaudio/include/audio/*.h $(ROOT)/usr/include/audio/
	@cp -p ../libedit/include/edit.h $(ROOT)/usr/include/
	@cp -p $(LUA_SRC)/lua.h $(LUA_SRC)/luaconf.h $(LUA_SRC)/lauxlib.h $(LUA_SRC)/lualib.h $(ROOT)/usr/include/lua/
	@for l in $(STATIC_LIBS); do cp -p $(BUILD)/$$l/$$l.a $(ROOT)/lib/$$l.a; done
	@cp -p $(TCC_SRC)/include/*.h $(ROOT)/usr/lib/tcc/include/
	@for t in $(TCC_TEST_NAMES); do cp -p $(TCC_TESTS)/$$t.c $(TCC_TESTS)/$$t.expect $(ROOT)/usr/share/tcc/tests2/; done
	@cp -p $(TCC_TESTS)/18_include.h $(TCC_TESTS)/18_include2.h $(ROOT)/usr/share/tcc/tests2/

# bin/ld is named explicitly: the pattern rule for program directories
# would match the loader's directory ld/ for that name.
$(ROOT)/bin/ld: coreutils/ld.c $(SOS) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/ld.elf $(ULDFLAGS) $(CRT0) $< $(DYNLIBS) -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/ld.elf $@

-include $(TCC_GEN)/tcc.d

# The loader is a freestanding position independent object with its own
# entry; init is linked statically, so that the system starts even when
# /lib is damaged.
$(ROOT)/lib/ld.so: ld/ld.c ld/arch/$(ARCH)/start.S ld/arch/$(ARCH)/ld_arch.h
	@mkdir -p $(dir $@) $(OUT)/ld
	$(CC) -std=c17 -ffreestanding -fno-stack-protector -fPIC -fvisibility=hidden -fno-plt -O2 -g \
	    -fno-builtin -Wall -Wextra $(LDSO_ARCHFLAGS) -I../kernel/include -I../libc/include -Ild/arch/$(ARCH) -c -o $(OUT)/ld/ld.o ld/ld.c
	$(CC) $(UASFLAGS) -c -o $(OUT)/ld/start.o ld/arch/$(ARCH)/start.S
	$(LD) $(USOFLAGS) -Bsymbolic -e _dl_start -o $(OUT)/ld.so.elf $(OUT)/ld/start.o $(OUT)/ld/ld.o $(LIBGCC)
	$(OBJCOPY) --strip-debug $(OUT)/ld.so.elf $@

$(ROOT)/lib/%.so: $(BUILD)/lib/%.so
	@mkdir -p $(dir $@)
	$(OBJCOPY) --strip-debug $< $@

$(ROOT)/bin/init: init/*.c $(LIBC) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/init.elf $(ULDFLAGS_STATIC) $(CRT0) $(filter %.c,$^) $(LIBC) -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/init.elf $@

$(ROOT)/bin/%: %/*.c $(SOS) $(LIBEDIT) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/$*.elf $(ULDFLAGS) $(CRT0) $(filter %.c,$^) $(DYNLIBS) -lgcc
	$(INSTALL)

# X12 keeps its implementation in compositor/ for source-history
# continuity; the installed server and user-facing name are X12.
$(ROOT)/bin/x12: compositor/*.c $(SOS) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/x12.elf $(ULDFLAGS) $(CRT0) $(filter %.c,$^) $(DYNLIBS) -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/x12.elf $@

$(ROOT)/bin/x12settings: apps/compsettings.c $(SOS) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/x12settings.elf $(ULDFLAGS) $(CRT0) $< $(DYNLIBS) -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/x12settings.elf $@

# Named explicitly so that the generated block table is a prerequisite;
# the generic apps rule below tracks only the .c file.
$(APPBIN)/unicode: apps/unicode.c apps/unicode_blocks.h $(SOS) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/unicode.elf $(ULDFLAGS) $(CRT0) $< $(DYNLIBS) -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/unicode.elf $@

$(ROOT)/bin/%: coreutils/%.c $(SOS) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/$*.elf $(ULDFLAGS) $(CRT0) $< $(DYNLIBS) -lgcc
	$(INSTALL)

$(ROOT)/bin/%: apps/%.c $(SOS) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/$*.elf $(ULDFLAGS) $(CRT0) $< $(DYNLIBS) -lgcc
	$(INSTALL)

$(ROOT)/bin/%: tests/%.c $(SOS) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/$*.elf $(ULDFLAGS) $(CRT0) $< $(DYNLIBS) -lgcc
	$(INSTALL)

clean:
	rm -rf $(OUT) $(ROOT)

# Executable loader fixtures are kept together with their source and the
# ELF mutation generator; they run only in the targeted dynlink test.
include ld/tests/fixtures.mk
include pkg/tests/fixtures.mk

include packages/packages.mk
