# Optional applications never pass through the base image's bin/ tree.
# Source manifests supply metadata. Packages default to the OS release number,
# with explicit versions for independently updated applications.
PKG_NAMES := calc code gedit hexview luasynth mandel paint player playtone pong sequencer synth unicode view
PKG_VERSION := $(shell cat ../VERSION)
PKG_VERSION_luasynth := 0.1.1
pkg_version = $(or $(PKG_VERSION_$(1)),$(PKG_VERSION))
PKG_OUT := $(BUILD)/packages
PKG_FILES := $(foreach a,$(PKG_NAMES),$(PKG_OUT)/$(a)-$(call pkg_version,$(a)).mpk)

.PHONY: packages prune-packaged
all: packages prune-packaged
packages: $(PKG_FILES)

$(APPBIN)/%: %/*.c $(SOS) $(LIBEDIT) $(CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/$*.elf $(ULDFLAGS) $(CRT0) $(filter %.c,$^) $(DYNLIBS) -lgcc
	$(INSTALL)

$(APPBIN)/%: apps/%.c $(SOS) $(LIBEDIT) $(CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/$*.elf $(ULDFLAGS) $(CRT0) $< $(DYNLIBS) -lgcc
	$(INSTALL)

$(APPBIN)/code: coreutils/code.c ../libc/include/minios/local.h $(SOS) $(CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/code.elf $(ULDFLAGS) $(CRT0) $< $(DYNLIBS) -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/code.elf $@

$(APPBIN)/synth $(APPBIN)/sequencer: apps/synthvoice.h

$(APPBIN)/luasynth: packages/luasynth/launcher.c ../libc/include/minios/local.h $(BUILD)/lib/libc.so $(CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UCPP) -o $(OUT)/luasynth.elf $(ULDFLAGS) $(CRT0) $< -lc -lgcc
	$(OBJCOPY) --strip-debug $(OUT)/luasynth.elf $@

define APP_PACKAGE
$(PKG_OUT)/$(1)-$(call pkg_version,$(1)).mpk: $(APPBIN)/$(1) packages/$(1)/manifest $(shell find packages/$(1)/files -type f 2>/dev/null) $(ROOT)/lib/abi ../tools/mkpkg.sh ../VERSION packages/packages.mk
	@rm -rf $(OUT)/packages/$(1)
	@mkdir -p $(OUT)/packages/$(1)/files/bin $(PKG_OUT)
	@cat packages/$(1)/manifest > $(OUT)/packages/$(1)/manifest
	@printf 'version $(call pkg_version,$(1))\n' >> $(OUT)/packages/$(1)/manifest
	@if [ -d packages/$(1)/files ]; then cp -Rp packages/$(1)/files/. $(OUT)/packages/$(1)/files/; fi
	@cp $(APPBIN)/$(1) $(OUT)/packages/$(1)/files/bin/$(1)
	READELF=$(READELF) ../tools/mkpkg.sh $(OUT)/packages/$(1) $$@ $(ROOT)
endef
$(foreach a,$(PKG_NAMES),$(eval $(call APP_PACKAGE,$(a))))

# Keep an offline archive shelf on the image. Replacing only this generated
# directory also drops old release archives on incremental builds.
packages:
	@rm -rf $(ROOT)/usr/share/packages
	@mkdir -p $(ROOT)/usr/share/packages
	@cp $(PKG_FILES) $(ROOT)/usr/share/packages/

# make repo writes the signed repository of the same archives anew each
# time, which keeps it to the current archives.
.PHONY: repo
repo: $(PKG_FILES)
	../tools/mkrepo.sh $(PKGSIGN) $(PKG_KEY_FILE) $(REPO) $(PKG_FILES)

# Remove the old built-in copies on incremental builds, including their
# default desktop shortcuts. Never touch the persistent data volume.
prune-packaged: share-tree skel-tree
	@rm -f $(addprefix $(ROOT)/bin/,$(PKG_NAMES))
	@rm -f $(ROOT)/usr/share/apps/code.lua $(ROOT)/usr/share/apps/pong.lua $(ROOT)/usr/share/man/man1/code.1
	@rm -f $(foreach d,etc/skel home/user root,$(ROOT)/$(d)/desktop/Code.app $(ROOT)/$(d)/desktop/Pong.app)

ifneq ($(WITH_TESTS),)
all: $(ROOT)/etc/tests/luasynth-engine.lua
$(ROOT)/etc/tests/luasynth-engine.lua: packages/luasynth/tests/engine.lua
	@mkdir -p $(dir $@)
	@cp $< $@

all: $(ROOT)/etc/tests/luasynth-profile.lua
$(ROOT)/etc/tests/luasynth-profile.lua: packages/luasynth/tests/profile.lua
	@mkdir -p $(dir $@)
	@cp $< $@

all: $(ROOT)/etc/tests/luasynth-worker.lua
$(ROOT)/etc/tests/luasynth-worker.lua: packages/luasynth/tests/worker.lua
	@mkdir -p $(dir $@)
	@cp $< $@
endif
