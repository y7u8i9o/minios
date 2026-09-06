# Fixtures of the package installer test (tests/cases/pkg): a library in
# two builds, one without the symbol the program uses, the program that
# loads it from the package prefix, and a package built on the host by
# tools/mkpkg.sh from the hello program.
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

$(ROOT)/etc/tests/pkghello-1.0.mpk: ../tools/mkpkg.sh pkg/tests/pkghello.manifest $(ROOT)/bin/hello $(ROOT)/lib/abi
	@rm -rf $(PKGFIX_OUT)/pkghello
	@mkdir -p $(PKGFIX_OUT)/pkghello/files/bin
	@cp pkg/tests/pkghello.manifest $(PKGFIX_OUT)/pkghello/manifest
	@cp $(ROOT)/bin/hello $(PKGFIX_OUT)/pkghello/files/bin/pkghello
	READELF=$(READELF) ../tools/mkpkg.sh $(PKGFIX_OUT)/pkghello $@ $(ROOT)
