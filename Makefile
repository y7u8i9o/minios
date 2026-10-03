include toolchain.mk

TOP      := $(CURDIR)
# The x86_64 build is in build/, and another architecture builds into
# build/$(ARCH)/, which lets both exist side by side.
ifeq ($(ARCH),x86_64)
BUILD    := $(TOP)/build
else
BUILD    := $(TOP)/build/$(ARCH)
endif
KERNEL   := $(BUILD)/kernel.elf
ISO      := $(BUILD)/minios.iso
INITRD   := $(BUILD)/initrd.tar
DISK     := $(BUILD)/disk.img
DISK_MB  ?= 512
SWAP     := $(BUILD)/swap.img
SWAP_MB  ?= 64
# The data volume contains /home with the installed packages, which are
# built for one machine, and each architecture therefore has its own
# volume.
ifeq ($(ARCH),x86_64)
DATA     ?= $(TOP)/data.img
else
DATA     ?= $(TOP)/data-$(ARCH).img
endif
DATA_MB  ?= 256
LIMINE   := $(BUILD)/host/limine
GENSYMS  := $(BUILD)/host/gensyms
MKFS     := $(BUILD)/host/mkfs
FSCK     := $(BUILD)/host/fsck
MKFAT    := $(BUILD)/host/mkfat
NETPEER  := $(BUILD)/host/netpeer
PKGSIGN  := $(BUILD)/host/pkgsign
PKGHOST  := $(BUILD)/host/pkg
MSGFMT   := $(BUILD)/host/msgfmt
# PKG_KEY names the key that signs the package repository index
# (docs/design/packages.md). The build generates one under build/ unless
# PKG_KEY names another file, and its public half is installed as
# /etc/pkg/keys/build.pub.
PKG_KEY  ?= $(BUILD)/pkg/signing.key
PKG_KEY_FILE := $(abspath $(PKG_KEY))
PKG_PUB  := $(BUILD)/pkg/signing.pub
# The repository of each architecture is a directory of build/repo, and
# one HTTP server serves both (docs/design/packages.md).
REPO     := $(TOP)/build/repo/$(ARCH)

export ARCH TOP BUILD KERNEL LIMINE GENSYMS INITRD DISK MKFS FSCK MKFAT NETPEER SWAP DATA PKGSIGN PKGHOST MSGFMT PKG_KEY_FILE PKG_PUB REPO

.PHONY: all kernel libc libfont libwire libaudio libcodec libgui user initrd disk image run gdb test test-kvm check clean clean-data tools repo release check-pkg $(DISK)

all: kernel libc user

tools: $(LIMINE) $(GENSYMS) $(MKFS) $(FSCK) $(MKFAT) $(NETPEER) $(PKGSIGN) $(PKGHOST) $(MSGFMT)

# msgfmt compiles the message catalogues of user/po (docs/design/gettext.md).
$(MSGFMT): tools/msgfmt/msgfmt.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCPPFLAGS) -O2 -std=c99 -Wall -o $@ $<

$(MKFS): tools/mkfs/mkfs.c kernel/include/fs/mfs_format.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCPPFLAGS) -O2 -std=c99 -Wall -Ikernel/include -o $@ $<

$(FSCK): tools/fsck/fsck.c kernel/include/fs/mfs_format.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCPPFLAGS) -O2 -std=c99 -Wall -Ikernel/include -o $@ $<

$(MKFAT): tools/mkfat/mkfat.c kernel/include/fs/fat_format.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCPPFLAGS) -O2 -std=c99 -Wall -Ikernel/include -o $@ $<

# The controlled peer of the network boot tests (docs/design/network.md).
$(NETPEER): tools/netpeer/netpeer.c tools/netpeer/scripted.c tools/netpeer/scripted.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCPPFLAGS) -O2 -std=c99 -Wall -o $@ tools/netpeer/netpeer.c tools/netpeer/scripted.c

# pkgsign generates keys and signs the index of a package repository. It
# compiles the SHA-2 and Ed25519 code of libc, with which pkg verifies on
# minios.
CRYPTO_SRCS := libc/src/crypto/sha2.c libc/src/crypto/ed25519.c libc/src/crypto/shacrypt.c
CRYPTO_HDRS := libc/include/minios/sha2.h libc/include/minios/ed25519.h
$(PKGSIGN): tools/pkgsign/pkgsign.c $(CRYPTO_SRCS) $(CRYPTO_HDRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCPPFLAGS) -O2 -std=c17 -Wall -Wextra -idirafter libc/include -o $@ tools/pkgsign/pkgsign.c $(CRYPTO_SRCS)

# The host build of pkg installs packages into the tree of an image with
# --root and prints the owners mkfs gives the files with pkg perms
# (docs/design/packages.md).
PKGHOST_SRCS := $(wildcard user/pkg/*.c) libc/src/gzip.c libc/src/net/http.c libc/src/crypto/sha2.c libc/src/crypto/ed25519.c
$(PKGHOST): $(PKGHOST_SRCS) user/pkg/pkg.h $(CRYPTO_HDRS) libc/include/minios/local.h libc/include/minios/gzip.h libc/include/minios/http.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCPPFLAGS) -O2 -std=c17 -Wall -Wextra -idirafter libc/include -o $@ $(PKGHOST_SRCS)

$(PKG_KEY_FILE): | $(PKGSIGN)
	@mkdir -p $(dir $@)
	$(PKGSIGN) keygen $@

# The public key is derived from the key on every run and replaced only
# when it changes, which lets another PKG_KEY install its public half.
.PHONY: FORCE
$(PKG_PUB): $(PKG_KEY_FILE) $(PKGSIGN) FORCE
	@mkdir -p $(dir $@)
	@$(PKGSIGN) public $(PKG_KEY_FILE) > $@.tmp
	@if cmp -s $@.tmp $@; then rm $@.tmp; else mv $@.tmp $@; fi

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

libcodec: libc
	$(MAKE) -C libcodec

libgui: libc libcodec libfont libwire
	$(MAKE) -C libgui

libedit: libc
	$(MAKE) -C libedit

packages: user

.PHONY: packages

user: libc libfont libwire libaudio libcodec libgui libedit $(PKG_PUB) $(MSGFMT)
	$(MAKE) -C user

# make repo writes the package repository of the bundled applications to
# build/repo/$(ARCH), with the archives, the index and its signature.
# `python3 -m http.server -d build/repo 8000` serves it to a guest as
# http://10.0.2.2:8000/$(ARCH), the URL of the shipped /etc/pkg.conf.
repo: user $(PKGSIGN) $(PKG_KEY_FILE)
	$(MAKE) -C user repo

# The initrd is a ustar archive of build/initrd_root, populated by user/.
initrd: user
	cd $(BUILD)/initrd_root && tar --format ustar --owner=0 --group=0 --exclude .DS_Store --exclude ./usr/share/sounds -cf $(INITRD) .

# The disk image is attached as a virtio-blk device and holds the root
# filesystem, an mfs image built from build/initrd_root (M13). It is
# rebuilt whenever a user program changes, which discards files written
# during earlier runs. Files keep their permission bits and belong to root,
# and the manifest user/perms sets the exceptions (docs/design/users.md).
$(DISK): $(MKFS) user user/perms
	@mkdir -p $(dir $@)
	$(MKFS) -p user/perms $@ $(DISK_MB) $(BUILD)/initrd_root

# Swap lives on a second virtio-blk device (M14), zero filled.
$(SWAP):
	@mkdir -p $(dir $@)
	dd if=/dev/zero of=$@ bs=1048576 count=$(SWAP_MB) status=none

# The data volume keeps the home directory across boots and across rebuilds
# of the root image. It is created once, empty, outside build/, and is
# never rebuilt, while `make clean-data` removes it.
$(DATA): | $(MKFS)
	@mkdir -p $(BUILD)/empty
	$(MKFS) $@ $(DATA_MB) $(BUILD)/empty

clean-data:
	rm -f $(DATA)

disk: $(DISK) $(SWAP) $(DATA)

# QEMU is started by tools/run.sh, which reads qemu.conf, QEMU_* variables
# and RUNFLAGS. Variables given on the make command line are exported, and
# `make QEMU_AUDIO=none run` keeps working through them. `run` and `gdb` let
# the script build the image, since the framebuffer mode it picks (VIDEO,
# or QEMU_VIDEO, or a doubled mode on a Retina display) is baked into it.
RUN := tools/run.sh
$(foreach v,QEMU QEMU_AUDIO QEMU_AUDIO_OPTS QEMU_WAV QEMU_SOUND QEMU_MEM QEMU_SMP \
            QEMU_ACCEL QEMU_DISPLAY QEMU_FULLSCREEN QEMU_VIDEO QEMU_SERIAL QEMU_EXTRA \
            QEMU_CONF CMDLINE, \
    $(if $(filter command line,$(origin $(v))),$(eval export $(v))))
ifeq ($(origin VIDEO),command line)
export QEMU_VIDEO := $(VIDEO)
endif

# VIDEO=WxH[xBPP][@SCALE] selects the framebuffer mode (video= on the
# kernel command line), and @2 doubles every pixel for high density
# displays.
image: kernel initrd $(LIMINE) $(DISK) $(SWAP) $(DATA)
	LIMINE=$(LIMINE) INITRD=$(INITRD) tools/mkiso.sh $(KERNEL) $(ISO) "$(strip $(CMDLINE) $(if $(VIDEO),video=$(VIDEO)))"

run:
	ISO=$(ISO) DISK=$(DISK) SWAP=$(SWAP) DATA=$(DATA) $(RUN) --build $(RUNFLAGS)

gdb:
	ISO=$(ISO) DISK=$(DISK) SWAP=$(SWAP) DATA=$(DATA) $(RUN) --build --gdb $(RUNFLAGS)

# CASES="gui gui_wm" runs only those cases, because the whole suite takes
# too long to run for every change.
ifeq ($(ARCH_USERLAND),yes)
test: kernel initrd $(LIMINE) $(DISK) $(FSCK) $(MKFAT) $(NETPEER) $(PKGSIGN)
	@LIMINE=$(LIMINE) INITRD=$(INITRD) DISK=$(DISK) MKFS=$(MKFS) MKFAT=$(MKFAT) NETPEER=$(NETPEER) tests/run_all.sh $(KERNEL) $(BUILD)/tests tests/cases $(CASES)
else
# An architecture without user programs boots the kernel with an empty
# initrd and no disk, where only kernel self tests can run.
test: kernel $(LIMINE) $(MKFAT)
	@mkdir -p $(BUILD)/empty/dev $(BUILD)/empty/tmp && tar --format ustar -cf $(INITRD) -C $(BUILD)/empty .
	@LIMINE=$(LIMINE) INITRD=$(INITRD) DISK= MKFAT=$(MKFAT) tests/run_all.sh $(KERNEL) $(BUILD)/tests tests/cases $(CASES)
endif

# The host self test of the network peer lifecycle of the boot harness
# runs a fake QEMU and the real peer tool without a guest
# (docs/design/network.md).
.PHONY: check-net
check-net: kernel initrd $(LIMINE) $(NETPEER)
	@LIMINE=$(LIMINE) INITRD=$(INITRD) NETPEER=$(NETPEER) tests/net/selftest.sh $(BUILD)

# Host fuzzing of the wire parsers and socket validators linked into the
# kernel, under the sanitizers (docs/design/network.md, N09). FUZZ_SECONDS
# bounds each of the three seeds (default 30).
.PHONY: check-net-fuzz
check-net-fuzz:
	@tests/net/fuzz/run.sh $(BUILD)/network-fuzz

# The cases whose behaviour depends on the processor or the hypervisor run
# with hardware virtualization, which needs Linux with /dev/kvm.
KVM_CASES := boot cpu exception fork signals smp smp_user vmm sched
test-kvm:
	ACCEL=kvm $(MAKE) test CASES="$(KVM_CASES)"

# check runs the host unit tests of the GUI framework, the Lua modules and
# the package signature code.
check: check-headers check-pkg
	$(MAKE) -C libfont check
	$(MAKE) -C libwire check
	$(MAKE) -C libcodec check
	$(MAKE) -C libgui check
	$(MAKE) check-lua
	$(MAKE) check-imed

.PHONY: check-lua check-headers check-imed
# The engines of the input method daemon on the host, with the
# dictionaries of user/share/ime (docs/design/ime.md).
check-imed:
	@mkdir -p $(BUILD)/host
	$(HOSTCC) $(HOSTCPPFLAGS) -D_DEFAULT_SOURCE -std=c17 -O1 -g -Wall -o $(BUILD)/host/test_pinyin user/imed/pycore.c \
	    user/imed/tests/test_pinyin.c -lm
	$(BUILD)/host/test_pinyin user/share/ime/pinyin.dict
	$(HOSTCC) $(HOSTCPPFLAGS) -D_DEFAULT_SOURCE -std=c17 -O1 -g -Wall -o $(BUILD)/host/test_japanese user/imed/jpcore.c \
	    user/imed/tests/test_japanese.c
	$(BUILD)/host/test_japanese user/share/ime/japanese.dict

# Every installed header must compile on its own with the cross compiler,
# in C17, as tcc will see it on minios (docs/design/tcc.md).
check-headers:
	@mkdir -p $(BUILD)/headers
	@status=0; for h in $$(cd libc/include && find . -name '*.h' | sed 's|^\./||') \
	    $$(cd libgui/include && find . -name '*.h' | sed 's|^\./||') font/font.h wire/client.h wire/common.h wire/server.h audio/audio.h \
	    codec/codec.h; do \
	    printf '#include <%s>\nint check_header_%s;\n' "$$h" "$$(echo $$h | tr -c 'A-Za-z0-9_\n' '_')" > $(BUILD)/headers/t.c; \
	    $(CC) $(UCFLAGS) -Wno-unused-parameter -Ilibc/include -Ikernel/include -Ilibgui/include -Ilibcodec/include -Ilibfont/include -Ilibwire/include -Ilibaudio/include -Ilibedit/include -fsyntax-only $(BUILD)/headers/t.c \
	        || { echo "header $$h does not compile alone"; status=1; }; \
	done; exit $$status
# The host unit test of the Lua modules compiles the interpreter, user/lua
# and the MIME code with the system compiler and runs the same script as
# the lua_sys boot test on a scratch directory.
LUA_HOSTSRCS := $(filter-out third_party/lua/src/lua.c third_party/lua/src/luac.c third_party/lua/src/linit.c,$(wildcard third_party/lua/src/*.c)) \
                $(wildcard user/lua/*.c) user/lua/tests/host_main.c user/lua/tests/fake_audio.c \
                $(filter-out libgui/src/client.c,$(wildcard libgui/src/*.c libgui/src/widgets/*.c)) \
                libgui/tests/fake_client.c libgui/tests/host_compat.c $(wildcard libfont/src/*.c) \
                $(wildcard libcodec/src/*.c libcodec/modules/*/*.c)
check-lua:
	@mkdir -p $(BUILD)/lua/host
	$(MAKE) -C libgui $(BUILD)/libgui/font.c
	$(HOSTCC) $(HOSTCPPFLAGS) -D_DEFAULT_SOURCE -D_GNU_SOURCE -DMINIOS_HOST -DCODEC_BUILTIN -DLUA_USE_POSIX -std=c17 -O1 -g -Wall \
	    -include libgui/tests/host_compat.h -Ithird_party/lua/src -Iuser/lua -Ilibgui/include -Ilibcodec/include -Ilibfont/include \
	    -Ilibgui/tests -Ilibaudio/include -idirafter kernel/include \
    -o $(BUILD)/lua/host/test_modules $(LUA_HOSTSRCS) $(BUILD)/libgui/font.c -lm -pthread
	rm -rf $(BUILD)/lua/host/tmp && mkdir -p $(BUILD)/lua/host/tmp
	$(BUILD)/lua/host/test_modules user/etc/tests/modules.lua $(BUILD)/lua/host/tmp user/etc/mime.types user/etc/mime.apps
	$(BUILD)/lua/host/test_modules user/lua/tests/gui.lua
	$(BUILD)/lua/host/test_modules user/lua/tests/audio.lua
	$(BUILD)/lua/host/test_modules user/etc/tests/threads.lua
	$(BUILD)/lua/host/test_modules user/packages/luasynth/tests/engine.lua
	$(BUILD)/lua/host/test_modules user/packages/luasynth/tests/ui.lua
	$(BUILD)/lua/host/test_modules user/packages/luasynth/tests/worker.lua

# check-pkg tests the SHA-256, SHA-512 and Ed25519 code on the host with
# the vectors of RFC 6234 and RFC 8032, and the pkg_repo case runs the same
# test on minios. It then runs the host build of pkg against an
# installation root in build/host/pkgtest.
check-pkg: $(PKGHOST)
	@mkdir -p $(BUILD)/host
	$(HOSTCC) $(HOSTCPPFLAGS) -O1 -g -std=c17 -Wall -Wextra -idirafter libc/include \
	    -o $(BUILD)/host/cryptotest user/tests/cryptotest.c $(CRYPTO_SRCS)
	$(BUILD)/host/cryptotest
	PKG=$(abspath $(PKGHOST)) WORK=$(abspath $(BUILD)/host/pkgtest) sh user/pkg/tests/host.sh

.PHONY: check-sh libedit
check-sh:
	@mkdir -p $(BUILD)/sh/host
	$(HOSTCC) $(HOSTCPPFLAGS) -DSH_TEST -D_DEFAULT_SOURCE -std=c17 -O1 -g -Wall -Wextra \
	    -Wno-unused-parameter -include user/sh/tests/host_compat.h -Iuser/sh \
	    -o $(BUILD)/sh/host/test_parser user/sh/*.c user/sh/tests/test_parser.c
	$(BUILD)/sh/host/test_parser

# The release pipeline builds the release that VERSION names from a commit,
# main by default, in a worktree of its own (tools/release.sh,
# docs/design/build.md). RELEASE_FLAGS passes options such as --ref, --arch
# or --tag to it.
release:
	tools/release.sh $(RELEASE_FLAGS)

clean:
	rm -rf $(BUILD)
