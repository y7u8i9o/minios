# The fixtures of the package installer test (tests/cases/pkg) are a
# library in two builds, one without the symbol the program uses, the
# program that loads it from /usr/lib, and a package built on
# the host by tools/mkpkg.sh from the hello program.
PKGFIX := $(ROOT)/etc/tests/pkgfix
PKGFIX_OUT := $(OUT)/pkgfix
all: $(PKGFIX)/libpkgfix.so $(PKGFIX)/bad/libpkgfix.so $(PKGFIX)/pkgprog $(ROOT)/etc/tests/pkghello-1.0.mpk

$(PKGFIX_OUT)/good.o: pkg/tests/fixture.c
	@mkdir -p $(PKGFIX_OUT)
	$(CC) $(UCFLAGS) $(UCPP) -c -o $@ $<

$(PKGFIX_OUT)/bad.o: pkg/tests/fixture.c
	@mkdir -p $(PKGFIX_OUT)
	$(CC) $(UCFLAGS) $(UCPP) -DWITHOUT_VALUE -c -o $@ $<

$(PKGFIX)/libpkgfix.so: $(PKGFIX_OUT)/good.o $(BUILD)/lib/libc.so
	@mkdir -p $(dir $@)
	$(LD) $(USOFLAGS) -soname libpkgfix.so -o $@ $< -L$(BUILD)/lib -lc $(LIBGCC)

$(PKGFIX)/bad/libpkgfix.so: $(PKGFIX_OUT)/bad.o $(BUILD)/lib/libc.so
	@mkdir -p $(dir $@)
	$(LD) $(USOFLAGS) -soname libpkgfix.so -o $@ $< -L$(BUILD)/lib -lc $(LIBGCC)

$(PKGFIX)/pkgprog: pkg/tests/prog.c $(PKGFIX)/libpkgfix.so $(CRT0)
	$(CC) $(UCFLAGS) $(UCPP) -o $@ $(ULDFLAGS) $(CRT0) $< -L$(PKGFIX) -lpkgfix -lc -lgcc

$(ROOT)/etc/tests/pkghello-1.0.mpk: ../tools/mkpkg.sh pkg/tests/pkghello.manifest $(ROOT)/usr/bin/hello $(BUILD)/lib/abi
	@rm -rf $(PKGFIX_OUT)/pkghello
	@mkdir -p $(PKGFIX_OUT)/pkghello/files/usr/bin
	@cp pkg/tests/pkghello.manifest $(PKGFIX_OUT)/pkghello/manifest
	@cp $(ROOT)/usr/bin/hello $(PKGFIX_OUT)/pkghello/files/usr/bin/pkghello
	READELF=$(READELF) ../tools/mkpkg.sh $(PKGFIX_OUT)/pkghello $@ $(BUILD)/lib/abi

# These rules build the archives of the repository test
# (tests/cases/pkg_repo). They stay on the host, where the case's peer
# assembles repositories from them. repohello comes in two versions, each
# with a NEWS file naming its version. repolib provides libpkgfix.so.
# repoprog needs that library and depends on repohello, which lets its
# installation resolve both through the index.
PKGREPO := $(OUT)/pkgrepo
all: $(PKGREPO)/repohello-1.0.mpk $(PKGREPO)/repohello-1.1.mpk $(PKGREPO)/repolib-1.0.mpk $(PKGREPO)/repoprog-1.0.mpk

$(PKGREPO)/repohello-%.mpk: ../tools/mkpkg.sh pkg/tests/repohello.manifest $(ROOT)/usr/bin/hello $(BUILD)/lib/abi
	@rm -rf $(PKGFIX_OUT)/repohello-$*
	@mkdir -p $(PKGFIX_OUT)/repohello-$*/files/usr/bin $(PKGFIX_OUT)/repohello-$*/files/usr/share/repohello $(dir $@)
	@cp pkg/tests/repohello.manifest $(PKGFIX_OUT)/repohello-$*/manifest
	@printf 'version $*\n' >> $(PKGFIX_OUT)/repohello-$*/manifest
	@printf 'repohello $*\n' > $(PKGFIX_OUT)/repohello-$*/files/usr/share/repohello/NEWS
	@cp $(ROOT)/usr/bin/hello $(PKGFIX_OUT)/repohello-$*/files/usr/bin/repohello
	READELF=$(READELF) ../tools/mkpkg.sh $(PKGFIX_OUT)/repohello-$* $@ $(BUILD)/lib/abi

$(PKGREPO)/repolib-1.0.mpk: ../tools/mkpkg.sh pkg/tests/repolib.manifest $(PKGFIX)/libpkgfix.so $(BUILD)/lib/abi
	@rm -rf $(PKGFIX_OUT)/repolib
	@mkdir -p $(PKGFIX_OUT)/repolib/files/usr/lib $(dir $@)
	@cp pkg/tests/repolib.manifest $(PKGFIX_OUT)/repolib/manifest
	@printf 'version 1.0\n' >> $(PKGFIX_OUT)/repolib/manifest
	@cp $(PKGFIX)/libpkgfix.so $(PKGFIX_OUT)/repolib/files/usr/lib/libpkgfix.so
	READELF=$(READELF) ../tools/mkpkg.sh $(PKGFIX_OUT)/repolib $@ $(BUILD)/lib/abi

$(PKGREPO)/repoprog-1.0.mpk: ../tools/mkpkg.sh pkg/tests/repoprog.manifest $(PKGFIX)/pkgprog $(BUILD)/lib/abi
	@rm -rf $(PKGFIX_OUT)/repoprog
	@mkdir -p $(PKGFIX_OUT)/repoprog/files/usr/bin $(dir $@)
	@cp pkg/tests/repoprog.manifest $(PKGFIX_OUT)/repoprog/manifest
	@printf 'version 1.0\n' >> $(PKGFIX_OUT)/repoprog/manifest
	@cp $(PKGFIX)/pkgprog $(PKGFIX_OUT)/repoprog/files/usr/bin/repoprog
	READELF=$(READELF) ../tools/mkpkg.sh $(PKGFIX_OUT)/repoprog $@ $(BUILD)/lib/abi
