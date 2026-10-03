include ../toolchain.mk

OUT    := $(BUILD)/user
ROOT   := $(BUILD)/initrd_root
LIBC   := $(BUILD)/libc/libc.a
LIBEDIT := $(BUILD)/libedit/libedit.a
CRT0   := $(BUILD)/libc/crt0.o
# The shared libraries (docs/design/dynlink.md). Programs name them with
# -l and --as-needed keeps only the ones a program references. libedit is
# static, since a single program uses it.
SHARED := libc libcodec libfont libwire libaudio libgui liblua
SOS    := $(addprefix $(BUILD)/lib/,$(addsuffix .so,$(SHARED)))
DYNLIBS := -lgui -lcodec -laudio -lwire -lfont $(LIBEDIT) -lc
# Every program is installed without its debugging sections, while the
# symbol table stays for the profiler.
INSTALL = $(OBJCOPY) --strip-debug $(OUT)/$*.elf $@

# Every directory holding a main program gives one binary named after it,
# and each file of tests/*.c becomes one binary as well.
PROGS  := init sh hello edit mint term x12 panel desktop audiod settings files pkg imed greeter
TESTS  := $(patsubst tests/%.c,%,$(wildcard tests/*.c))
# A build with CONFIG_TESTS=0, which the release pipeline uses, installs no
# test programs, test fixtures or test data (docs/design/build.md).
WITH_TESTS := $(filter 1,$(strip $(CONFIG_TESTS)))
UTILS  := $(filter-out code,$(patsubst coreutils/%.c,%,$(wildcard coreutils/*.c)))
APPS   := clock evtest logview profiler screenshot sysmon wireview x12settings
APPBIN := $(OUT)/app-bin
BINS   := $(addprefix $(ROOT)/bin/,$(PROGS) $(if $(WITH_TESTS),$(TESTS)) $(UTILS) $(APPS) lua luac sed awk make tar tcc doas su sudo)
LIBS   := $(addprefix $(ROOT)/lib/,ld.so abi $(addsuffix .so,$(SHARED)))
# The codec modules (docs/design/codecs.md), loaded by libcodec at run time.
CODECS := $(addprefix $(ROOT)/lib/codecs/,$(addsuffix .so,$(notdir $(wildcard ../libcodec/modules/*))))

# Lua is compiled unmodified from third_party/lua/src, with the platform
# choices made here instead of in luaconf.h (see docs/design/lua.md).
# LUA_USE_POSIX selects _setjmp, getc_unlocked, fseeko, mkstemp, popen
# and the wait status macros, all of which libc provides. Modules load
# only from the share tree and the current directory, without dynamic
# loading. lua/ holds the replacement linit.c and the minios
# modules (fs, sys), compiled with the interpreter headers.
LUA_SRC  := ../third_party/lua/src
LUA_CORE := $(filter-out $(LUA_SRC)/lua.c $(LUA_SRC)/luac.c $(LUA_SRC)/linit.c,$(wildcard $(LUA_SRC)/*.c))
LUA_CORE_OBJS := $(patsubst $(LUA_SRC)/%.c,$(OUT)/lua/%.o,$(LUA_CORE))
LUA_OBJS := $(patsubst lua/%.c,$(OUT)/lua/minios_%.o,$(wildcard lua/*.c))
LUA_CPP  := -DLUA_USE_POSIX -DLUA_PATH_DEFAULT='"/usr/share/lua/5.5/?.lua;/usr/share/lua/5.5/?/init.lua;./?.lua;./?/init.lua"' \
            -DLUA_CPATH_DEFAULT='""'

UCPP   := -I../libc/include -I../kernel/include -I../libcodec/include -I../libgui/include -I../libfont/include -I../libwire/include -I../libwire/generated -I../libaudio/include
UCPP   += -I../libedit/include

ETC    := $(patsubst etc/%,$(ROOT)/etc/%,$(filter-out $(if $(WITH_TESTS),,etc/tests etc/tests/%),$(wildcard etc/* etc/*/*)))
FONTS  := $(ROOT)/etc/fonts/DejaVuSans.ttf $(ROOT)/etc/fonts/DejaVuSansMono.ttf $(ROOT)/etc/fonts/lmroman10-regular.otf $(ROOT)/etc/fonts/NotoSans-Regular.ttf $(ROOT)/etc/fonts/unifont.otf \
          $(ROOT)/etc/fonts/DroidSansFallbackFull.ttf

.PHONY: all clean
all: $(LIBS) $(CODECS) $(BINS) $(ETC) share-tree locale-tree skel-tree $(FONTS) $(ROOT)/dev $(ROOT)/initrd $(ROOT)/tmp tcc-support links modes

$(ROOT)/etc/%: etc/%
	@mkdir -p $(dir $@)
	@if [ -f $< ]; then cp $< $@; else mkdir -p $@; fi

# Two symbolic links that the symlink test resolves on the root image and
# under /initrd, which mkfs and the initrd reader store as links. One is
# relative and leads to the tar fixture beside it, and the other is
# absolute and leads to /etc/motd. The account databases in /etc lead to the package prefix
# /usr/local, itself a link to /home/.local on the data volume, which
# keeps accounts across rebuilds of the root image (docs/design/users.md).
# They are recreated on every build.
.PHONY: links
links: $(ETC)
	@if [ -n "$(WITH_TESTS)" ]; then ln -sfn fixture.tar $(ROOT)/etc/tests/link-fixture.tar; fi
	@if [ -n "$(WITH_TESTS)" ]; then ln -sfn /etc/motd $(ROOT)/etc/tests/link-motd; fi
	@for f in passwd group shadow; do ln -sfn /usr/local/etc/$$f $(ROOT)/etc/$$f; done
	@mkdir -p $(ROOT)/usr && ln -sfn /home/.local $(ROOT)/usr/local

# pkg-key installs the public half of the key that signs the package
# repository index (docs/design/packages.md), which the top level Makefile
# derives. The copy compares contents, since a change of PKG_KEY can leave
# timestamps equal to the second.
.PHONY: pkg-key
all: pkg-key
pkg-key:
	@mkdir -p $(ROOT)/etc/pkg/keys
	@cmp -s $(PKG_PUB) $(ROOT)/etc/pkg/keys/build.pub || cp $(PKG_PUB) $(ROOT)/etc/pkg/keys/build.pub

# The share tree is copied whole, because make cannot name files with
# spaces, and media files may carry them.
# Every po/DOMAIN/LL.po becomes /usr/share/locale/LL/LC_MESSAGES/DOMAIN.mo
# (docs/design/gettext.md).
.PHONY: locale-tree
locale-tree:
	@for po in po/*/*.po; do \
	    [ -f "$$po" ] || continue; \
	    domain=$$(basename $$(dirname $$po)); lang=$$(basename $$po .po); \
	    mkdir -p $(ROOT)/usr/share/locale/$$lang/LC_MESSAGES; \
	    $(MSGFMT) -o $(ROOT)/usr/share/locale/$$lang/LC_MESSAGES/$$domain.mo $$po || exit 1; \
	done

.PHONY: share-tree
share-tree:
	@mkdir -p $(ROOT)/usr/share/lua/5.5
	@cp -Rp share/. $(ROOT)/usr/share/
	@if [ -d ../third_party/fontawesome/svgs ]; then cp ../third_party/fontawesome/svgs/*.svg $(ROOT)/usr/share/icons/; fi

# The home skeleton skel/ is installed as /etc/skel, from which useradd
# and fsinit make new homes, and copied into the homes of the two accounts
# of the image, /root and /home/user, for a boot without a data volume.
# local/ holds the account databases of the package prefix /home/.local
# (/usr/local), which go to the root image and, with the layout marker,
# into /usr/share/skel/home, the seed of a fresh data volume (etc/fstab,
# docs/design/users.md). Files of the single user layout that earlier
# builds left behind are removed.
.PHONY: skel-tree
skel-tree:
	@rm -rf $(ROOT)/home/desktop $(ROOT)/home/.shrc $(ROOT)/usr/share/skel/home/desktop \
	    $(ROOT)/usr/share/skel/home/.shrc
	@for d in $(ROOT)/etc/skel $(ROOT)/home/user $(ROOT)/root; do mkdir -p $$d && cp -Rp skel/. $$d/; done
	@for d in $(ROOT)/home $(ROOT)/usr/share/skel/home; do \
	    mkdir -p $$d/.local && cp -Rp local/. $$d/.local/ && echo 2 > $$d/.layout; done

# Modes that differ from those of the sources, set on the build tree, from
# which both mkfs and the initrd archive take them. Owners other than root
# come from the manifest perms.
.PHONY: modes
modes: $(BINS) $(ETC) skel-tree $(ROOT)/tmp $(ROOT)/var/log $(ROOT)/var/run $(ROOT)/var/db
	@chmod 1777 $(ROOT)/tmp
	@chmod 0700 $(ROOT)/root $(ROOT)/home/user
	@chmod 0600 $(ROOT)/home/.local/etc/shadow $(ROOT)/usr/share/skel/home/.local/etc/shadow
	@chmod 4755 $(ROOT)/bin/su $(ROOT)/bin/passwd $(ROOT)/bin/doas $(ROOT)/bin/sudo
	@chmod 0440 $(ROOT)/etc/sudoers

$(ROOT)/etc/fonts/%.ttf: ../third_party/dejavu/%.ttf
	@mkdir -p $(dir $@)
	cp $< $@

$(ROOT)/etc/fonts/%.otf: ../third_party/lmodern/%.otf
	@mkdir -p $(dir $@)
	cp $< $@

$(ROOT)/etc/fonts/NotoSans-Regular.ttf: ../third_party/notosans/NotoSans-Regular.ttf
	@mkdir -p $(dir $@)
	cp $< $@

# The fallback font for Chinese and Japanese text (tools/fetch_fonts.sh).
$(ROOT)/etc/fonts/DroidSansFallbackFull.ttf: ../third_party/droidfallback/DroidSansFallbackFull.ttf
	@mkdir -p $(dir $@)
	cp $< $@

# The font is installed without its release number, which keeps an upgrade
# of Unifont from touching the paths compiled into the Unicode viewer. The
# release is recorded in third_party/unifont/NOTICE.
$(ROOT)/etc/fonts/unifont.otf: ../third_party/unifont/unifont-17.0.05.otf
	@mkdir -p $(dir $@)
	cp $< $@

$(ROOT)/dev $(ROOT)/initrd $(ROOT)/tmp $(ROOT)/var/log $(ROOT)/var/run $(ROOT)/var/db:
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

# The ABI table that the package installer reads holds one "soname number"
# line per shared library, taken from the ABI variables of the library
# Makefiles.
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
# (LUAI_FUNC in luaconf.h), and it therefore links the core objects
# directly.
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
# the utility library files it needs (see docs/design/artar.md). The
# strlcpy of sbase is renamed to avoid a collision with the one of libc.
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

# doas is OpenDoas, compiled unmodified from third_party/opendoas with the
# shadow file authentication, a config.h written here in place of its
# configure script, and the functions of its libopenbsd that libc lacks
# (docs/design/users.md). parse.y goes through the host yacc.
DOAS_SRC  := ../third_party/opendoas
DOAS_GEN  := $(OUT)/doas
DOAS_COMPAT := closefrom.c execvpe.c explicit_bzero.c reallocarray.c strtonum.c readpassphrase.c
DOAS_OBJS := $(DOAS_GEN)/doas.o $(DOAS_GEN)/env.o $(DOAS_GEN)/shadow.o $(DOAS_GEN)/parse.o \
             $(patsubst %.c,$(DOAS_GEN)/compat_%.o,$(DOAS_COMPAT))
DOAS_CFLAGS := $(PORT_CFLAGS) -D__minios__ -Wno-sign-compare -Wno-unused-parameter -Wno-implicit-fallthrough \
               -I$(DOAS_GEN) -I$(DOAS_SRC) -I$(DOAS_SRC)/libopenbsd

$(DOAS_GEN)/config.h:
	@mkdir -p $(dir $@)
	@printf '%s\n' '#ifndef CONFIG_H' '#define CONFIG_H' '#define HAVE_ERRC' '#define HAVE_VERRC' \
	    '#define HAVE_SETRESUID' '#define HAVE_SETRESGID' '#define HAVE_STRLCAT' '#define HAVE_STRLCPY' \
	    '#define HAVE_SETPROGNAME' '#define USE_SHADOW' '#define UID_MAX 4294967295U' \
	    '#define GID_MAX 4294967295U' '#define DOAS_CONF "/etc/doas.conf"' '#endif' > $@

$(DOAS_GEN)/parse.c: $(DOAS_SRC)/parse.y
	@mkdir -p $(dir $@)
	$(YACC) -o $@ $<

$(DOAS_GEN)/%.o: $(DOAS_SRC)/%.c $(DOAS_GEN)/config.h
	$(CC) $(UCFLAGS) $(DOAS_CFLAGS) $(UCPP) -MMD -MP -c -o $@ $<

$(DOAS_GEN)/parse.o: $(DOAS_GEN)/parse.c $(DOAS_GEN)/config.h
	$(CC) $(UCFLAGS) $(DOAS_CFLAGS) -Wno-unused-function $(UCPP) -MMD -MP -c -o $@ $<

$(DOAS_GEN)/compat_%.o: $(DOAS_SRC)/libopenbsd/%.c $(DOAS_GEN)/config.h
	$(CC) $(UCFLAGS) $(DOAS_CFLAGS) $(UCPP) -MMD -MP -c -o $@ $<

$(ROOT)/bin/doas: $(DOAS_OBJS) $(BUILD)/lib/libc.so $(CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -o $(OUT)/doas.elf $(ULDFLAGS) $(CRT0) $(DOAS_OBJS) -lc -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/doas.elf $@

-include $(DOAS_OBJS:.o=.d)

# su is the su of ubase, the suckless Linux base utilities, compiled
# unmodified from third_party/ubase with the parts of its utility library
# it uses (docs/design/users.md).
SU_SRC  := ../third_party/ubase/src
SU_OBJS := $(OUT)/su/su.o $(OUT)/su/passwd.o $(OUT)/su/eprintf.o

$(OUT)/su/su.o: $(SU_SRC)/su.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(PORT_CFLAGS) $(UCPP) -I$(SU_SRC) -MMD -MP -c -o $@ $<

$(OUT)/su/%.o: $(SU_SRC)/libutil/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(PORT_CFLAGS) $(UCPP) -I$(SU_SRC) -MMD -MP -c -o $@ $<

$(ROOT)/bin/su: $(SU_OBJS) $(BUILD)/lib/libc.so $(CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -o $(OUT)/su.elf $(ULDFLAGS) $(CRT0) $(SU_OBJS) -lc -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/su.elf $@

-include $(SU_OBJS:.o=.d)

# sudo is sudo 1.9.17p1, compiled unmodified from third_party/sudo with the
# sudoers policy linked in statically. Its configure script ran once
# against the minios libc with the cross compiler, and the headers it
# generated, with the signal name table, are kept in ports/sudo
# (docs/design/users.md). The object lists follow the makefiles of the
# configured tree, covering the front end of src, the sudoers plugin and
# its parser and authentication, the event and I/O log libraries,
# protobuf-c and the utility library with the replacement functions that
# configure selected.
SUDO_SRC  := ../third_party/sudo
SUDO_CFG  := ports/sudo
SUDO_UTIL := basename digest event fatal key_val gethostname gettime getgrouplist gidlist hexchar \
              json lbuf locking logfac login_max logpri mkdir_parents mmap_alloc multiarch parseln \
              progname rcstr regex roundup secure_path setgroups strsplit strtobool strtoid strtomode \
              strtonum sudo_conf sudo_debug sudo_dso term ttyname_dev ttysize uuid event_poll pread \
              pwrite getusershell reallocarray arc4random arc4random_uniform getentropy utimens \
              explicit_bzero fnmatch memrchr freezero mkdirat unlinkat dup3 pw_dup getopt_long \
              closefrom mktemp sha2 sig2str str2sig
SUDO_FRONT := conversation copy_file edit_open env_hooks exec exec_common exec_intercept exec_iolog \
              exec_monitor exec_nopty exec_preload exec_ptrace exec_pty get_pty hooks limits \
              load_plugins net_ifs parse_args preserve_fds signal sudo sudo_edit suspend_parent \
              tgetpass ttyname utmp preload
SUDO_AUTH := sudo_auth passwd
SUDO_POLICY := getspwuid audit boottime check check_util display editor env \
              sudoers_hooks env_pattern file find_path fmtsudoers gc goodpath group_plugin interfaces \
              iolog iolog_path_escapes locale log_client logging lookup policy prompt serialize_list \
              set_perms sethost starttime strlcpy_unesc strvec_join sudo_nss sudoers sudoers_cb \
              sudoers_ctx_free timestamp unesc_str alias b64_decode canon_path defaults digestname \
              exptilde filedigest gentime gram match match_addr match_command match_digest \
              parser_warnx pwutil pwutil_impl redblack resolve_cmnd strlist sudoers_debug timeout \
              timestr toke toke_util
SUDO_IOLOG := host_port hostcheck iolog_clearerr iolog_close iolog_conf iolog_eof iolog_filter \
              iolog_flush iolog_gets iolog_json iolog_legacy iolog_loginfo iolog_mkdirs iolog_mkdtemp \
              iolog_mkpath iolog_nextid iolog_open iolog_openat iolog_path iolog_read iolog_seek \
              iolog_swapids iolog_timing iolog_util iolog_write
SUDO_EVLOG := eventlog eventlog_conf eventlog_free logwrap parse_json
SUDO_OBJS := $(patsubst %,$(OUT)/sudo/util/%.o,$(SUDO_UTIL)) $(OUT)/sudo/util/signame.o \
             $(patsubst %,$(OUT)/sudo/src/%.o,$(SUDO_FRONT)) \
             $(patsubst %,$(OUT)/sudo/sudoers/%.o,$(SUDO_POLICY)) \
             $(patsubst %,$(OUT)/sudo/auth/%.o,$(SUDO_AUTH)) \
             $(patsubst %,$(OUT)/sudo/iolog/%.o,$(SUDO_IOLOG)) \
             $(patsubst %,$(OUT)/sudo/eventlog/%.o,$(SUDO_EVLOG)) $(OUT)/sudo/protobuf-c/protobuf-c.o
SUDO_CFLAGS := $(UCFLAGS) -std=gnu17 $(PORT_CFLAGS) -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function \
               -Wno-missing-field-initializers -Wno-cast-function-type $(UCPP) -I$(SUDO_CFG) -I$(SUDO_SRC)/include \
               -DZLIB_CONST -D_PATH_SUDO_CONF=\"/etc/sudo.conf\" -DLOCALEDIR=\"/usr/share/locale\"

$(OUT)/sudo/util/%.o: $(SUDO_SRC)/lib/util/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SUDO_CFLAGS) -I$(SUDO_SRC)/lib/util -DDEFAULT_TEXT_DOMAIN=\"sudo\" -MMD -MP -c -o $@ $<

$(OUT)/sudo/util/signame.o: $(SUDO_CFG)/signame.c
	@mkdir -p $(dir $@)
	$(CC) $(SUDO_CFLAGS) -I$(SUDO_SRC)/lib/util -MMD -MP -c -o $@ $<

$(OUT)/sudo/src/%.o: $(SUDO_SRC)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SUDO_CFLAGS) -I$(SUDO_SRC)/src -MMD -MP -c -o $@ $<

$(OUT)/sudo/sudoers/%.o: $(SUDO_SRC)/plugins/sudoers/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SUDO_CFLAGS) -I$(SUDO_SRC)/plugins/sudoers -DLIBDIR=\"/usr/lib\" -D_PATH_SUDOERS=\"/etc/sudoers\" \
	    -D_PATH_CVTSUDOERS_CONF=\"/etc/cvtsudoers.conf\" -DSUDOERS_UID=0 -DSUDOERS_GID=0 -DSUDOERS_MODE=0440 \
	    -MMD -MP -c -o $@ $<

$(OUT)/sudo/auth/%.o: $(SUDO_SRC)/plugins/sudoers/auth/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SUDO_CFLAGS) -I$(SUDO_SRC)/plugins/sudoers -MMD -MP -c -o $@ $<

$(OUT)/sudo/iolog/%.o: $(SUDO_SRC)/lib/iolog/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SUDO_CFLAGS) -I$(SUDO_SRC)/lib/iolog -MMD -MP -c -o $@ $<

$(OUT)/sudo/eventlog/%.o: $(SUDO_SRC)/lib/eventlog/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SUDO_CFLAGS) -I$(SUDO_SRC)/lib/eventlog -I$(SUDO_SRC) -MMD -MP -c -o $@ $<

$(OUT)/sudo/protobuf-c/%.o: $(SUDO_SRC)/lib/protobuf-c/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SUDO_CFLAGS) -I$(SUDO_SRC) -MMD -MP -c -o $@ $<

$(ROOT)/bin/sudo: $(SUDO_OBJS) $(BUILD)/lib/libc.so $(CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -o $(OUT)/sudo.elf $(ULDFLAGS) $(CRT0) $(SUDO_OBJS) -lc -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/sudo.elf $@

-include $(SUDO_OBJS:.o=.d)

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

# crt1.o is the C runtime start, crti.o defines the empty _init and _fini
# and crtn.o is empty, and tcc links these objects around every program.
# libm.a is empty because the math functions are in libc.so.
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

# A selection of the tcc test suite runs on minios (tests/cases/tcc),
# namely the programs that need neither arguments, input files, bound
# checking, threads, thread local storage nor a specific assembler.
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
# library, where a program compiled on minios can use the toolkit, the
# protocol library, the audio client, the line editor and the Lua core
# with the same headers and the same objects as the cross build.
STATIC_LIBS := libc libcodec libgui libfont libwire libaudio libedit
tcc-support: $(ROOT)/usr/lib/tcc/libtcc1.a $(ROOT)/usr/lib/tcc/runmain.o $(ROOT)/lib/crt1.o $(ROOT)/lib/crti.o $(ROOT)/lib/crtn.o $(ROOT)/lib/libm.a
	@mkdir -p $(ROOT)/usr/include/minios $(ROOT)/usr/include/codec $(ROOT)/usr/include/gui $(ROOT)/usr/include/font $(ROOT)/usr/include/wire \
	    $(ROOT)/usr/include/audio $(ROOT)/usr/include/lua $(ROOT)/usr/lib/tcc/include
	@cp -Rp ../libc/include/. $(ROOT)/usr/include/
	@cp -p ../kernel/include/minios/*.h $(ROOT)/usr/include/minios/
	@cp -p ../kernel/include/syscall_nums.h $(ROOT)/usr/include/
	@cp -p ../libgui/include/gui/*.h $(ROOT)/usr/include/gui/
	@cp -p ../libfont/include/font/*.h $(ROOT)/usr/include/font/
	@cp -p ../libwire/include/wire/*.h $(ROOT)/usr/include/wire/
	@cp -p ../libaudio/include/audio/*.h $(ROOT)/usr/include/audio/
	@cp -p ../libcodec/include/codec/*.h $(ROOT)/usr/include/codec/
	@cp -p ../libedit/include/edit.h $(ROOT)/usr/include/
	@cp -p $(LUA_SRC)/lua.h $(LUA_SRC)/luaconf.h $(LUA_SRC)/lauxlib.h $(LUA_SRC)/lualib.h $(ROOT)/usr/include/lua/
	@for l in $(STATIC_LIBS); do cp -p $(BUILD)/$$l/$$l.a $(ROOT)/lib/$$l.a; done
	@cp -p $(TCC_SRC)/include/*.h $(ROOT)/usr/lib/tcc/include/
ifneq ($(WITH_TESTS),)
	@mkdir -p $(ROOT)/usr/share/tcc/tests2
	@for t in $(TCC_TEST_NAMES); do cp -p $(TCC_TESTS)/$$t.c $(TCC_TESTS)/$$t.expect $(ROOT)/usr/share/tcc/tests2/; done
	@cp -p $(TCC_TESTS)/18_include.h $(TCC_TESTS)/18_include2.h $(ROOT)/usr/share/tcc/tests2/
endif

# bin/ld is named explicitly, because the pattern rule for program
# directories would match the loader's directory ld/ for that name.
$(ROOT)/bin/ld: coreutils/ld.c $(SOS) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/ld.elf $(ULDFLAGS) $(CRT0) $< $(DYNLIBS) -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/ld.elf $@

-include $(TCC_GEN)/tcc.d

# The loader is a freestanding position independent object with its own
# entry. init is linked statically, which lets the system start even when
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

# X12 keeps its implementation in compositor/ for the continuity of the
# source history, while the installed server and its visible name are X12.
$(ROOT)/bin/x12: compositor/*.c $(SOS) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/x12.elf $(ULDFLAGS) $(CRT0) $(filter %.c,$^) $(DYNLIBS) -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/x12.elf $@

$(ROOT)/bin/x12settings: apps/compsettings.c $(SOS) $(CRT0)
	@mkdir -p $(dir $@) $(OUT)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/x12settings.elf $(ULDFLAGS) $(CRT0) $< $(DYNLIBS) -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/x12settings.elf $@

# This program is named explicitly to make the generated block table a
# prerequisite, because the generic rule below tracks only the .c file.
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
# ELF mutation generator, and they run only in the dynlink test.
ifneq ($(WITH_TESTS),)
include ld/tests/fixtures.mk
include pkg/tests/fixtures.mk
endif

include packages/packages.mk
