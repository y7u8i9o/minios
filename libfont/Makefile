include ../toolchain.mk

OUT   := $(BUILD)/libfont
LIB   := $(OUT)/libfont.a
SO    := $(BUILD)/lib/libfont.so
# The ABI number of the shared object (docs/design/packages.md). It is
# raised whenever a structure, constant, signature or documented behaviour
# that programs depend on changes incompatibly, and additions leave it
# alone.
ABI   := 1
ABIFILE := $(BUILD)/lib/libfont.abi
SRCS  := $(wildcard src/*.c)
OBJS  := $(patsubst %.c,$(OUT)/%.o,$(SRCS))
UCPP  := -Iinclude -I../libc/include -I../kernel/include

.PHONY: all clean check
all: $(LIB) $(SO) $(ABIFILE)

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UCPP) -MMD -MP -c -o $@ $<

$(SO): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(USOFLAGS) -soname libfont.so -o $@ $(OBJS) -L$(BUILD)/lib -lc $(LIBGCC)

$(LIB): $(OBJS)
	rm -f $@
	$(AR) rcs $@ $(OBJS)

# Host test of the rasterizer against the bundled DejaVu font.
check: tests/test_raster.c $(SRCS)
	@mkdir -p $(OUT)/host
	$(HOSTCC) $(HOSTCPPFLAGS) -std=c11 -O1 -Wall -Iinclude -o $(OUT)/host/test_raster tests/test_raster.c $(SRCS)
	$(OUT)/host/test_raster ../third_party/dejavu/DejaVuSans.ttf

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)

$(ABIFILE): Makefile
	@mkdir -p $(dir $@)
	@echo $(ABI) > $@
