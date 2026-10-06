include ../../toolchain.mk

OUT   := $(BUILD)/libgui
LIB   := $(OUT)/libgui.a
SO    := $(BUILD)/lib/libgui.so
# The ABI number of the shared object (docs/design/packages.md). It is
# raised whenever a structure, constant, signature or documented behaviour
# that programs depend on changes incompatibly, and additions leave it
# alone.
ABI   := 1
ABIFILE := $(BUILD)/lib/libgui.abi
SRCS  := $(wildcard src/*.c src/widgets/*.c)
OBJS  := $(patsubst %.c,$(OUT)/%.o,$(SRCS)) $(OUT)/font.o
UCPP  := -Iinclude -I../libcodec/include -I../libfont/include -I../libwire/include -I../libwire/generated -I../libc/include -I../../kernel/include

.PHONY: all clean check
all: $(LIB) $(SO) $(ABIFILE)

# The host unit tests compile the library, libcodec with its modules
# (CODEC_BUILTIN) and libfont with the system compiler against a fake
# client (tests/fake_client.c). The PNG encoder uses the CRC-32 of libc.
HOSTSRCS := $(filter-out src/client.c,$(SRCS)) $(wildcard ../libfont/src/*.c) $(wildcard tests/*.c) \
            $(wildcard ../libcodec/src/*.c ../libcodec/modules/*/*.c) ../libc/src/crc32.c
check: $(OUT)/font.c
	@mkdir -p $(OUT)/host
	$(HOSTCC) $(HOSTCPPFLAGS) -std=c17 -O1 -g -Wall -DMINIOS_HOST -DCODEC_BUILTIN -DPIXEL_EXHAUSTIVE -include tests/host_compat.h -Iinclude \
	    -I../libcodec/include -I../libfont/include -Itests \
	    -idirafter ../../kernel/include -idirafter ../libc/include \
	    -o $(OUT)/host/test_framework $(HOSTSRCS) $(OUT)/font.c
	$(OUT)/host/test_framework

# The font is the kernel's, with the array renamed for user space.
$(OUT)/font.c: ../../kernel/drivers/font.c
	@mkdir -p $(dir $@)
	sed -e 's|#include <drivers/font.h>|#include <gui/gfx.h>|' -e 's|font8x16\[256\]\[FONT_HEIGHT\]|gfx_font8x16[256][16]|' $< > $@

$(OUT)/font.o: $(OUT)/font.c
	$(CC) $(UCFLAGS) $(UCPP) -c -o $@ $<

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UCPP) -MMD -MP -c -o $@ $<

$(SO): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(USOFLAGS) -soname libgui.so -o $@ $(OBJS) -L$(BUILD)/lib -lcodec -lfont -lwire -lc $(LIBGCC)

$(LIB): $(OBJS)
	rm -f $@
	$(AR) rcs $@ $(OBJS)

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)

$(ABIFILE): Makefile
	@mkdir -p $(dir $@)
	@echo $(ABI) > $@
