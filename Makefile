include toolchain.mk

TOP      := $(CURDIR)
# The x86_64 build is written to build/. The build of any other
# architecture is written to build/$(ARCH)/, so the builds of both
# architectures can exist at the same time.
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
# The data volume contains /home. The packages that the user installs are
# stored in /home, and these packages are compiled for one architecture.
# Therefore each architecture has its own data volume.
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
# PKG_KEY is the file of the private key that signs the index of the
# package repository (docs/design/packages.md). The default file is
# build/pkg/signing.key. If the file does not exist, the build generates a
# new key in it. The matching public key is installed as
# /etc/pkg/keys/build.pub.
PKG_KEY  ?= $(BUILD)/pkg/signing.key
PKG_KEY_FILE := $(abspath $(PKG_KEY))
PKG_PUB  := $(BUILD)/pkg/signing.pub
# PKG_ORIGIN is the repository origin that the public key allows the key
# to sign (docs/design/packages.md). The indexes that tools/mkrepo.sh
# writes contain the origin in REPO_ORIGIN, which is also minios by
# default.
PKG_ORIGIN ?= minios
# Each architecture has its own repository directory in build/repo. One
# HTTP server serves both directories (docs/design/packages.md).
REPO     := $(TOP)/build/repo/$(ARCH)

export ARCH TOP BUILD KERNEL LIMINE GENSYMS INITRD DISK MKFS FSCK MKFAT MKGPT NETPEER SWAP DATA PKGSIGN PKGHOST MSGFMT READELF PKG_KEY_FILE PKG_PUB REPO

.PHONY: all kernel libc libfont libwire libaudio libcodec libgui libjson libprof user initrd disk image run gdb test test-kvm check clean clean-data tools repo release check-pkg $(DISK)

all: kernel libc user

tools: $(LIMINE) $(GENSYMS) $(MKFS) $(FSCK) $(MKFAT) $(MKGPT) $(NETPEER) $(PKGSIGN) $(PKGHOST) $(MSGFMT)

# msgfmt compiles the translation files in user/po into message
# catalogues (docs/design/gettext.md).
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

# netpeer runs on the host. In the network boot tests, the guest
# communicates with it (docs/design/network.md).
$(NETPEER): tools/netpeer/netpeer.c tools/netpeer/scripted.c tools/netpeer/scripted.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCPPFLAGS) -O2 -std=c99 -Wall -o $@ tools/netpeer/netpeer.c tools/netpeer/scripted.c

# pkgsign generates signing keys and signs the index of a package
# repository. It is compiled from the SHA-2 and Ed25519 code of libc. pkg
# uses the same code on minios to verify signatures.
CRYPTO_SRCS := libc/src/crypto/sha2.c libc/src/crypto/ed25519.c libc/src/crypto/shacrypt.c
CRYPTO_HDRS := libc/include/minios/sha2.h libc/include/minios/ed25519.h
$(PKGSIGN): tools/pkgsign/pkgsign.c $(CRYPTO_SRCS) $(CRYPTO_HDRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCPPFLAGS) -O2 -std=c17 -Wall -Wextra -idirafter libc/include -o $@ tools/pkgsign/pkgsign.c $(CRYPTO_SRCS)

# PKGHOST is pkg compiled for the host. With --root, it installs packages
# into the directory tree of a disk image. `pkg perms` prints the mode and
# the owner of every installed file, and mkfs sets them in the image
# (docs/design/packages.md).
PKGHOST_SRCS := $(wildcard user/pkg/*.c) libc/src/gzip.c libc/src/net/http.c libc/src/crypto/sha2.c libc/src/crypto/ed25519.c
$(PKGHOST): $(PKGHOST_SRCS) user/pkg/pkg.h $(CRYPTO_HDRS) libc/include/minios/local.h libc/include/minios/gzip.h libc/include/minios/http.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCPPFLAGS) -O2 -std=c17 -Wall -Wextra -idirafter libc/include -o $@ $(PKGHOST_SRCS)

$(PKG_KEY_FILE): | $(PKGSIGN)
	@mkdir -p $(dir $@)
	$(PKGSIGN) keygen $@

# The public key is computed from the private key on every build. The
# file is replaced only if its contents change. A different PKG_KEY
# therefore changes the installed public key, and an unchanged key causes
# no rebuild.
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

libjson: libc
	$(MAKE) -C libjson

libprof: libc libjson
	$(MAKE) -C libprof

packages: user

.PHONY: packages

user: libc libfont libwire libaudio libcodec libgui libedit libjson libprof $(PKG_PUB) $(MSGFMT)
	$(MAKE) -C user

# make repo writes the package repository of the bundled applications to
# build/repo/$(ARCH): the package archives, the index and the signature of
# the index. The command `python3 -m http.server -d build/repo 8000`
# serves the repository. A guest downloads it from
# http://10.0.2.2:8000/$(ARCH), which is the URL in the installed
# /etc/pkg.conf.
repo: user $(PKGSIGN) $(PKG_KEY_FILE)
	$(MAKE) -C user repo

# The kernel and the boot loader files belong to the packages kernel and
# limine (P6). These packages install the files under /boot, which is the
# EFI system partition on an installed disk.
# user/ installs the whole system into build/initrd_root. tools/mkbase.py
# then divides this tree into the packages of the base system according to
# the definitions in user/packages/*/paths, and writes the packages to
# build/base (docs/plan/packaging.md, P3). A package is packed again only
# if its files changed.
BASE     := $(BUILD)/base
# A development build adds a serial number to the version of each package,
# so a rebuilt package is an upgrade on the development disk. A release
# sets PKG_SERIAL=0, and every package then has the version in VERSION
# without a serial number.
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

# sysimage builds the root image and the initrd. PKGHOST installs every
# package of build/base into build/sysroot, except the metapackage apps.
# The applications in apps are installed later by the tests or by the user.
# The root image is an mfs filesystem (M13) on a virtio-blk disk. It is
# rebuilt on every build, so files written during earlier runs are lost.
# File owners and setuid bits are taken from `pkg perms` and user/perms
# (docs/design/users.md).
sysimage: base $(PKGHOST) $(MKFS) user/perms
	@mkdir -p $(dir $(DISK))
	tools/mkimage.sh $(PKGHOST) $(ARCH) $(BUILD)/initrd_root $(SYSROOT) $(DISK) $(DISK_MB) $(MKFS) $(INITRD) \
	    $$(ls $(BASE)/*.mpk | grep -v '/apps-[0-9.]*\.mpk$$')

initrd: sysimage

# The installation medium (P7, docs/design/installer.md) is a GPT disk. It
# contains the boot files, the installer environment and a signed
# repository with every package. QEMU attaches it as a virtio disk. For
# real hardware, it is written to a USB stick.
INSTALLER := $(BUILD)/installer.img
.PHONY: installer
installer: base kernel $(PKGHOST) $(PKGSIGN) $(PKG_KEY_FILE) $(PKG_PUB) $(MKFS) $(MKFAT) $(MKGPT) $(LIMINE)
	tools/mkinstaller.sh $(ARCH) $(BASE) $(BUILD)/packages $(INSTALLER) 1024

$(DISK): sysimage

# The swap area is a second virtio-blk device (M14), filled with zeros.
$(SWAP):
	@mkdir -p $(dir $@)
	dd if=/dev/zero of=$@ bs=1048576 count=$(SWAP_MB) status=none

# The data volume contains the home directories. Rebuilds of the root
# image do not change it, so its files remain after a reboot and after a
# rebuild. It is created once, empty, outside build/. Only
# `make clean-data` deletes it.
$(DATA): | $(MKFS)
	@mkdir -p $(BUILD)/empty
	$(MKFS) $@ $(DATA_MB) $(BUILD)/empty

clean-data:
	rm -f $(DATA)

disk: $(DISK) $(SWAP) $(DATA)

# tools/run.sh starts QEMU. It reads qemu.conf, the QEMU_* variables and
# RUNFLAGS. Variables set on the make command line are exported to it, so
# `make QEMU_AUDIO=none run` passes QEMU_AUDIO to tools/run.sh. For `run`
# and `gdb`, tools/run.sh builds the image itself, because the image
# contains the framebuffer mode that the script selects (VIDEO, QEMU_VIDEO,
# or a doubled mode on a Retina display).
RUN := tools/run.sh
$(foreach v,QEMU QEMU_AUDIO QEMU_AUDIO_OPTS QEMU_WAV QEMU_SOUND QEMU_MEM QEMU_SMP \
            QEMU_ACCEL QEMU_DISPLAY QEMU_FULLSCREEN QEMU_VIDEO QEMU_SERIAL QEMU_EXTRA \
            QEMU_CONF CMDLINE, \
    $(if $(filter command line,$(origin $(v))),$(eval export $(v))))
ifeq ($(origin VIDEO),command line)
export QEMU_VIDEO := $(VIDEO)
endif

# VIDEO=WxH[xBPP][@SCALE] selects the framebuffer mode. It is passed as
# video= on the kernel command line. @2 draws every pixel twice in each
# direction, for high density displays.
image: kernel initrd $(LIMINE) $(DISK) $(SWAP) $(DATA)
	LIMINE=$(LIMINE) INITRD=$(INITRD) tools/mkiso.sh $(KERNEL) $(ISO) "$(strip $(CMDLINE) $(if $(VIDEO),video=$(VIDEO)))"

# The development disk (P8, docs/design/packages.md) contains an installed
# system. tools/mkdisk.sh writes it once from the packages, with VIDEO on
# its kernel command line. make run attaches three disks: the development
# disk, the update medium of the current build, and the data volume, which
# is mounted on /home. At boot, pkg-update installs the packages of the
# update medium. With BOOT=kernel, QEMU boots the kernel of the current
# build from the CD instead of the kernel on the disk, and the root
# filesystem is the one on the development disk.
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

# run-image boots the image of the boot tests from the CD. This is how
# make run booted before P8.
run-image:
	ISO=$(ISO) DISK=$(DISK) SWAP=$(SWAP) DATA=$(DATA) $(RUN) --build $(RUNFLAGS)

gdb-image:
	ISO=$(ISO) DISK=$(DISK) SWAP=$(SWAP) DATA=$(DATA) $(RUN) --build --gdb $(RUNFLAGS)

# CASES="gui gui_wm" runs only the named cases. The whole suite takes too
# long to run after every change.
ifeq ($(ARCH_USERLAND),yes)
test: kernel initrd $(LIMINE) $(DISK) $(FSCK) $(MKFAT) $(MKGPT) $(NETPEER) $(PKGSIGN)
	@LIMINE=$(LIMINE) INITRD=$(INITRD) DISK=$(DISK) MKFS=$(MKFS) MKFAT=$(MKFAT) MKGPT=$(MKGPT) NETPEER=$(NETPEER) tests/run_all.sh $(KERNEL) $(BUILD)/tests tests/cases $(CASES)
else
# An architecture without user programs boots the kernel with an empty
# initrd and no disk. Only the kernel self tests can run there.
test: kernel $(LIMINE) $(MKFAT)
	@mkdir -p $(BUILD)/empty/dev $(BUILD)/empty/tmp && tar --format ustar -cf $(INITRD) -C $(BUILD)/empty .
	@LIMINE=$(LIMINE) INITRD=$(INITRD) DISK= MKFAT=$(MKFAT) tests/run_all.sh $(KERNEL) $(BUILD)/tests tests/cases $(CASES)
endif

# check-net tests on the host how the boot test script starts and stops
# netpeer. It runs the real netpeer with a fake QEMU and boots no guest
# (docs/design/network.md).
.PHONY: check-net
check-net: kernel initrd $(LIMINE) $(NETPEER)
	@LIMINE=$(LIMINE) INITRD=$(INITRD) NETPEER=$(NETPEER) tests/net/selftest.sh $(BUILD)

# check-net-fuzz fuzzes the IP, TCP and UDP packet parsers and validation
# functions of the kernel on the host, compiled with the sanitizers
# (docs/design/network.md, N09). FUZZ_SECONDS is the run time of each of
# the three seeds in seconds (default 30).
.PHONY: check-net-fuzz
check-net-fuzz:
	@tests/net/fuzz/run.sh $(BUILD)/network-fuzz

# These cases depend on the processor or on the hypervisor. test-kvm runs
# them with hardware virtualization, which requires Linux with /dev/kvm.
KVM_CASES := boot cpu exception fork signals smp smp_user vmm sched
test-kvm:
	ACCEL=kvm $(MAKE) test CASES="$(KVM_CASES)"

# check runs the tests on the host: the installed headers, the signature
# code and pkg, libfont, libwire, libcodec, libgui, the Lua modules and the
# input method engines.
check: check-headers check-pkg
	$(MAKE) -C libfont check
	$(MAKE) -C libwire check
	$(MAKE) -C libcodec check
	$(MAKE) -C libgui check
	$(MAKE) check-lua
	$(MAKE) check-imed

.PHONY: check-lua check-headers check-imed
# check-imed tests the engines of the input method daemon on the host with
# the dictionaries in user/share/ime (docs/design/ime.md).
check-imed:
	@mkdir -p $(BUILD)/host
	$(HOSTCC) $(HOSTCPPFLAGS) -D_DEFAULT_SOURCE -std=c17 -O1 -g -Wall -o $(BUILD)/host/test_pinyin user/imed/pycore.c \
	    user/imed/tests/test_pinyin.c -lm
	$(BUILD)/host/test_pinyin user/share/ime/pinyin.dict
	$(HOSTCC) $(HOSTCPPFLAGS) -D_DEFAULT_SOURCE -std=c17 -O1 -g -Wall -o $(BUILD)/host/test_japanese user/imed/jpcore.c \
	    user/imed/tests/test_japanese.c
	$(BUILD)/host/test_japanese user/share/ime/japanese.dict

# check-headers compiles every installed header alone with the cross
# compiler in C17, as tcc compiles it on minios (docs/design/tcc.md).
check-headers:
	@mkdir -p $(BUILD)/headers
	@status=0; for h in $$(cd libc/include && find . -name '*.h' | sed 's|^\./||') \
	    $$(cd libgui/include && find . -name '*.h' | sed 's|^\./||') font/font.h wire/client.h wire/common.h wire/server.h audio/audio.h \
	    codec/codec.h prof/profile.h; do \
	    printf '#include <%s>\nint check_header_%s;\n' "$$h" "$$(echo $$h | tr -c 'A-Za-z0-9_\n' '_')" > $(BUILD)/headers/t.c; \
	    $(CC) $(UCFLAGS) -Wno-unused-parameter -Ilibc/include -Ikernel/include -Ilibgui/include -Ilibcodec/include -Ilibfont/include -Ilibwire/include -Ilibaudio/include -Ilibedit/include -Ilibjson/include -Ilibprof/include -fsyntax-only $(BUILD)/headers/t.c \
	        || { echo "header $$h does not compile alone"; status=1; }; \
	done; exit $$status
# check-lua compiles the Lua interpreter and the modules in user/lua with
# the host compiler, together with libgui, libfont and libcodec. It runs
# the script of the lua_sys boot test in build/lua/host/tmp, and then the
# other Lua test scripts.
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
# the test vectors of RFC 6234 and RFC 8032. The boot test pkg_repo runs
# the same test on minios. check-pkg then tests PKGHOST with an
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

# make release builds the release with the version in VERSION. It builds
# a commit (main by default) in a separate worktree (tools/release.sh,
# docs/design/build.md). RELEASE_FLAGS passes options such as --ref,
# --arch or --tag to tools/release.sh.
release:
	tools/release.sh $(RELEASE_FLAGS)

clean:
	rm -rf $(BUILD)
