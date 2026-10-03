# The loader integration fixtures. The dependency graph is explicit, which
# keeps a parallel build from linking a node before its predecessor exists.
LDTEST := $(OUT)/ldtests
LDTEST_LEVELS := 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19
LDTEST_CHAIN := $(addprefix $(ROOT)/lib/libldchain,$(addsuffix .so,$(LDTEST_LEVELS)))
all: $(LDTEST_CHAIN) $(ROOT)/bin/ldlifecycle $(ROOT)/bin/ldstatic $(LDTEST)/fixtures.stamp \
     $(ROOT)/lib/libldtls.so $(ROOT)/lib/libldplugdep.so $(ROOT)/lib/libldplugin.so $(ROOT)/bin/ldlazy $(ROOT)/bin/dltest

# Thread local storage, dlopen and lazy binding (dltest). The plugin and
# its dependency are not linked into any program, and dlopen loads them.
$(LDTEST)/%.o: ld/tests/%.c
	@mkdir -p $(LDTEST)
	$(CC) $(UCFLAGS) $(UCPP) -c -o $@ $<

$(ROOT)/lib/libldtls.so: $(LDTEST)/tls.o $(BUILD)/lib/libc.so
	$(LD) $(USOFLAGS) -soname libldtls.so -o $@ $< -L$(BUILD)/lib -lc

$(ROOT)/lib/libldplugdep.so: $(LDTEST)/plugdep.o $(BUILD)/lib/libc.so
	$(LD) $(USOFLAGS) -soname libldplugdep.so -o $@ $< -L$(BUILD)/lib -lc

$(ROOT)/lib/libldplugin.so: $(LDTEST)/plugin.o $(ROOT)/lib/libldplugdep.so $(ROOT)/lib/libldtls.so
	$(LD) $(USOFLAGS) -z lazy -soname libldplugin.so -o $@ $< -L$(ROOT)/lib -lldplugdep -lldtls -L$(BUILD)/lib -lc

$(ROOT)/bin/ldlazy: ld/tests/lazy.c $(ROOT)/lib/libldtls.so $(CRT0)
	$(CC) $(UCFLAGS) $(UCPP) -o $@ $(ULDFLAGS) -Wl,-z,lazy $(CRT0) $< -L$(ROOT)/lib -lldtls -lc -lgcc

$(ROOT)/bin/dltest: tests/dltest.c $(ROOT)/lib/libldtls.so $(CRT0)
	$(CC) $(UCFLAGS) $(UCPP) -o $@ $(ULDFLAGS) -Wl,--export-dynamic $(CRT0) $< -L$(ROOT)/lib -lldtls -lc -lgcc

$(LDTEST)/chain0.o: ld/tests/chain.c
	@mkdir -p $(LDTEST)
	$(CC) $(UCFLAGS) $(UCPP) -DLEVEL=0 -c -o $@ $<

$(ROOT)/lib/libldchain0.so: $(LDTEST)/chain0.o $(BUILD)/lib/libc.so
	$(LD) $(USOFLAGS) --hash-style=gnu -soname libldchain0.so -init dyn_legacy_init -fini dyn_legacy_fini --defsym dyn_absolute_zero=0 -o $@ $< -L$(BUILD)/lib -lc

# Nodes alternate between GNU-only and dual hash tables. SysV-only lookup
# is exercised by libc and by the generated mutation fixtures.
define LDTEST_NODE
$(LDTEST)/chain$(1).o: ld/tests/chain.c
	@mkdir -p $(LDTEST)
	$(CC) $(UCFLAGS) $(UCPP) -DLEVEL=$(1) -DPREVIOUS=$(2) -c -o $$@ $$<
$(ROOT)/lib/libldchain$(1).so: $(LDTEST)/chain$(1).o $(ROOT)/lib/libldchain$(2).so
	$(LD) $(USOFLAGS) --hash-style=$(if $(filter 2 4 6 8 10 12 14 16 18,$(1)),both,gnu) -soname libldchain$(1).so -o $$@ $$< -L$(ROOT)/lib -lldchain$(2) -L$(BUILD)/lib -lc
endef
$(foreach n,1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19,$(eval $(call LDTEST_NODE,$(n),$(shell expr $(n) - 1))))

$(ROOT)/bin/ldlifecycle: ld/tests/lifecycle.c $(LDTEST_CHAIN) $(CRT0)
	$(CC) $(UCFLAGS) $(UCPP) -o $@ $(ULDFLAGS) -Wl,--export-dynamic -Wl,-rpath-link,$(ROOT)/lib -Wl,-rpath-link,$(BUILD)/lib $(CRT0) $< -L$(ROOT)/lib -lldchain19 -Wl,--no-as-needed -lldchain0 -lc -lgcc

$(ROOT)/bin/ldstatic: ld/tests/lifecycle.c $(LIBC) $(CRT0)
	$(CC) $(UCFLAGS) $(UCPP) -DSTATIC_PROBE -o $@ $(ULDFLAGS_STATIC) $(CRT0) $< $(LIBC) -lgcc

$(LDTEST)/fixture.o: ld/tests/fixture.c
	@mkdir -p $(LDTEST)
	$(CC) $(UCFLAGS) $(UCPP) -c -o $@ $<

$(LDTEST)/libldbad0000.so: $(LDTEST)/fixture.o ld/tests/fixtures.mk
	$(LD) $(USOFLAGS) -z noexecstack -z relro --hash-style=both -soname libldbad0000.so -o $@ $<

$(LDTEST)/fixture_main: ld/tests/fixture_main.c $(LDTEST)/libldbad0000.so $(CRT0) $(BUILD)/lib/libc.so
	$(CC) $(UCFLAGS) -fno-pic -fno-pie $(UCPP) -o $@ $(ULDFLAGS) $(CRT0) $< -L$(LDTEST) -lldbad0000 -lc -lgcc

$(LDTEST)/fixtures.stamp: ld/tests/fixtures.py $(LDTEST)/libldbad0000.so $(LDTEST)/fixture_main
	python3 ld/tests/fixtures.py $(LDTEST)/libldbad0000.so $(LDTEST)/fixture_main $(ROOT)
	@touch $@
