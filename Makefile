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
MKGPT    := $(BUILD)/host/mkgpt
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
# PKG_ORIGIN is the origin that the key may sign and that the indexes of
# the build name (docs/design/packages.md).
PKG_ORIGIN ?= minios
# The repository of each architecture is a directory of build/repo, and
# one HTTP server serves both (docs/design/packages.md).
REPO     := $(TOP)/build/repo/$(ARCH)

export ARCH TOP BUILD KERNEL LIMINE GENSYMS INITRD DISK MKFS FSCK MKFAT MKGPT NETPEER SWAP DATA PKGSIGN PKGHOST MSGFMT READELF PKG_KEY_FILE PKG_PUB REPO

.PHONY: all kernel libc libfont libwire libaudio libcodec libgui libprof user initrd disk image run gdb test test-kvm check clean clean-data tools repo release check-pkg $(DISK)

all: kernel libc user

tools: $(LIMINE) $(GENSYMS) $(MKFS) $(FSCK) $(MKFAT) $(MKGPT) $(NETPEER) $(PKGSIGN) $(PKGHOST) $(MSGFMT)

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

$(MKGPT): tools/mkgpt/mkgpt.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCPPFLAGS) -O2 -std=c99 -Wall -o $@ $<

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
	@$(PKGSIGN) public $(PKG_KEY_FILE) $(PKG_ORIGIN) > $@.tmp
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

libprof: libc
	$(MAKE) -C libprof

packages: user

.PHONY: packages

user: libc libfont libwire libaudio libcodec libgui libedit libprof $(PKG_PUB) $(MSGFMT)
	$(MAKE) -C user

# make repo writes the package repository of the bundled applications to
# build/repo/$(ARCH), with the archives, the index and its signature.
# `python3 -m http.server -d build/repo 8000` serves it to a guest as
# http://10.0.2.2:8000/$(ARCH), the URL of the shipped /etc/pkg.conf.
repo: user $(PKGSIGN) $(PKG_KEY_FILE)
	$(MAKE) -C user repo

# The kernel and the files of the boot loader belong to the packages
# kernel and limine (P6), which install them below /boot, the EFI system
# partition of an installed disk.
# user/ installs the whole system into build/initrd_root, and
# tools/mkbase.py splits that tree into the packages of the base system in
# build/base, following the definitions in user/packages/*/paths
# (docs/plan/packaging.md, P3). A package whose files did not change is
# not packed again.
BASE     := $(BUILD)/base
# A development build numbers its packages, which makes a rebuilt package
# an upgrade on the development disk. The release sets PKG_SERIAL=0 and
# gives every package the version of VERSION alone.
PKG_SERIAL ?= 1
SYSROOT  := $(BUILD)/sysroot
.PHONY: base sysimage
base: user kernel
	@mkdir -p $(BUILD)/initrd_root/boot/minios $(BUILD)/initrd_root/boot/EFI/BOOT
	@cmp -s $(KERNEL) $(BUILD)/initrd_root/boot/minios/kernel.elf || \
	    { cp $(KERNEL) $(BUILD)/initrd_root/boot/minios/kernel.elf && chmod 644 $(BUILD)/initrd_root/boot/minios/kernel.elf; }
ifeq ($(ARCH),x86_64)
	@mkdir -p $(BUILD)/initrd_root/boot/limine
	@cp -p third_party/limine/BOOTX64.EFI $(BUILD)/initrd_root/boot/EFI/BOOT/
	@cp -p third_party/limine/limine-bios.sys $(BUILD)/initrd_root/boot/limine/
else
	@cp -p third_party/limine/BOOTAA64.EFI $(BUILD)/initrd_root/boot/EFI/BOOT/
endif
	READELF=$(READELF) python3 tools/mkbase.py --root $(BUILD)/initrd_root --defs user/packages \
	    --abi $(BUILD)/lib/abi --out $(BASE) --version $(shell cat VERSION) $(if $(filter 1,$(strip $(CONFIG_TESTS))),,--skip tests) \
	    $(if $(filter 1,$(PKG_SERIAL)),--serial)

# The root image and the initrd contain the base system as the host build of
# pkg installs it into build/sysroot, every package of build/base except
# the metapackage apps, whose applications are installed by the tests and
# the user. The disk is attached as a virtio-blk device and contains the
# root filesystem, an mfs image (M13). It is rebuilt with every build,
# which discards files written during earlier runs. Owners and setuid bits
# come from pkg perms and user/perms (docs/design/users.md).
sysimage: base $(PKGHOST) $(MKFS) user/perms
	@mkdir -p $(dir $(DISK))
	tools/mkimage.sh $(PKGHOST) $(ARCH) $(BUILD)/initrd_root $(SYSROOT) $(DISK) $(DISK_MB) $(MKFS) $(INITRD) \
	    $$(ls $(BASE)/*.mpk | grep -v '/apps-[0-9.]*\.mpk$$')

initrd: sysimage

# The installation medium (P7, docs/design/installer.md): a GPT disk with
# the boot files, the installer environment and the signed repository of
# every package. QEMU attaches it as a virtio disk, and on hardware it is
# written to a USB stick.
INSTALLER := $(BUILD)/installer.img
.PHONY: installer
installer: base kernel $(PKGHOST) $(PKGSIGN) $(PKG_KEY_FILE) $(PKG_PUB) $(MKFS) $(MKFAT) $(MKGPT) $(LIMINE)
	tools/mkinstaller.sh $(ARCH) $(BASE) $(BUILD)/packages $(INSTALLER) 1024

$(DISK): sysimage

# Swap lives on a second virtio-blk device (M14), zero filled.
$(SWAP):
	@mkdir -p $(dir $@)
	dd if=/dev/zero of=$@ bs=1048576 count=$(SWAP_MB) status=none

# The data volume retains the home directory across boots and across rebuilds
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
# `make QEMU_AUDIO=none run` continues to work through them. `run` and `gdb` let
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

# The development disk (P8, docs/design/packages.md): an installed system
# that tools/mkdisk.sh writes once from the packages, with VIDEO on its
# kernel command line. make run attaches it with the update medium of
# the current build, whose packages pkg-update installs at boot, and the
# data volume on /home. BOOT=kernel boots the kernel of the build from the
# CD instead of the one on the disk, with the root of the disk.
DEVDISK  := $(BUILD)/dev.img
UPDATE   := $(BUILD)/update.img
DEVDISK_MB ?= 1024
.PHONY: devdisk updates devprep clean-devdisk run-image gdb-image
$(DEVDISK): | $(MKGPT) $(MKFS) $(MKFAT) $(LIMINE) $(PKGHOST)
	$(MAKE) sysimage
	uuid=$$(python3 -c 'import uuid; print(uuid.uuid4())'); \
	ROOT_UUID=$$uuid SWAP_MB=256 tools/mkdisk.sh $(PKGHOST) $(ARCH) $(SYSROOT) user/perms $@ $(DEVDISK_MB) \
	    $(MKFS) $(MKFAT) $(MKGPT) $(LIMINE) "$(if $(VIDEO),video=$(VIDEO))" && echo $$uuid > $@.root
devdisk: $(DEVDISK)
clean-devdisk:
	rm -f $(DEVDISK) $(DEVDISK).root
updates: base $(PKGSIGN) $(PKG_KEY_FILE) $(MKFS) $(MKGPT)
	VIDEO="$(VIDEO)" tools/mkupdate.sh $(ARCH) $(BASE) $(BUILD)/packages $(UPDATE)
devprep: updates $(DEVDISK) kernel $(LIMINE)
ifeq ($(BOOT),kernel)
	LIMINE=$(LIMINE) tools/mkiso.sh $(KERNEL) $(ISO) "root=PARTUUID=$$(cat $(DEVDISK).root) $(strip $(CMDLINE) $(if $(VIDEO),video=$(VIDEO)))"
endif

run:
	ISO=$(ISO) DEVDISK=$(DEVDISK) UPDATE=$(UPDATE) DATA=$(DATA) RUN_BOOT=$(BOOT) $(RUN) --build $(RUNFLAGS)

gdb:
	ISO=$(ISO) DEVDISK=$(DEVDISK) UPDATE=$(UPDATE) DATA=$(DATA) RUN_BOOT=$(BOOT) $(RUN) --build --gdb $(RUNFLAGS)

# The image of the boot tests with the CD, as make run booted it before P8.
run-image:
	ISO=$(ISO) DISK=$(DISK) SWAP=$(SWAP) DATA=$(DATA) $(RUN) --build $(RUNFLAGS)

gdb-image:
	ISO=$(ISO) DISK=$(DISK) SWAP=$(SWAP) DATA=$(DATA) $(RUN) --build --gdb $(RUNFLAGS)

# CASES="gui gui_wm" runs only those cases, because the whole suite takes
# too long to run for every change.
ifeq ($(ARCH_USERLAND),yes)
test: kernel initrd $(LIMINE) $(DISK) $(FSCK) $(MKFAT) $(MKGPT) $(NETPEER) $(PKGSIGN)
	@LIMINE=$(LIMINE) INITRD=$(INITRD) DISK=$(DISK) MKFS=$(MKFS) MKFAT=$(MKFAT) MKGPT=$(MKGPT) NETPEER=$(NETPEER) tests/run_all.sh $(KERNEL) $(BUILD)/tests tests/cases $(CASES)
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
	    codec/codec.h prof/profile.h; do \
	    printf '#include <%s>\nint check_header_%s;\n' "$$h" "$$(echo $$h | tr -c 'A-Za-z0-9_\n' '_')" > $(BUILD)/headers/t.c; \
	    $(CC) $(UCFLAGS) -Wno-unused-parameter -Ilibc/include -Ikernel/include -Ilibgui/include -Ilibcodec/include -Ilibfont/include -Ilibwire/include -Ilibaudio/include -Ilibedit/include -Ilibprof/include -fsyntax-only $(BUILD)/headers/t.c \
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
check-pkg: $(PKGHOST) $(PKGSIGN)
	@mkdir -p $(BUILD)/host
	$(HOSTCC) $(HOSTCPPFLAGS) -O1 -g -std=c17 -Wall -Wextra -idirafter libc/include \
	    -o $(BUILD)/host/cryptotest user/tests/cryptotest.c $(CRYPTO_SRCS)
	$(BUILD)/host/cryptotest
	PKG=$(abspath $(PKGHOST)) PKGSIGN=$(abspath $(PKGSIGN)) MKREPO=$(abspath tools/mkrepo.sh) WORK=$(abspath $(BUILD)/host/pkgtest) sh user/pkg/tests/host.sh

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
