include ../../toolchain.mk

# The profiler client (docs/design/profile.md): control of /dev/profile,
# symbol resolution and the aggregation the front ends display. It left
# libc in P2 of docs/plan/packaging.md, which lets it form the package
# profiler together with prof and the profiler window.
OUT   := $(BUILD)/libprof
LIB   := $(OUT)/libprof.a
SO    := $(BUILD)/lib/libprof.so
# The ABI number of the shared object (docs/design/packages.md). It is
# raised whenever a structure, constant, signature or documented behaviour
# that programs depend on changes incompatibly, and additions leave it
# alone.
ABI   := 1
ABIFILE := $(BUILD)/lib/libprof.abi
SRCS  := $(wildcard src/*.c)
OBJS  := $(patsubst %.c,$(OUT)/%.o,$(SRCS))
UCPP  := -Iinclude -I../libjson/include -I../libc/include -I../../kernel/include

.PHONY: all clean
all: $(LIB) $(SO) $(ABIFILE)

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UCPP) -MMD -MP -c -o $@ $<

$(SO): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(USOFLAGS) -soname libprof.so -o $@ $(OBJS) -L$(BUILD)/lib -ljson -lc $(LIBGCC)

$(LIB): $(OBJS)
	rm -f $@
	$(AR) rcs $@ $(OBJS)

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)

$(ABIFILE): Makefile
	@mkdir -p $(dir $@)
	@echo $(ABI) > $@
