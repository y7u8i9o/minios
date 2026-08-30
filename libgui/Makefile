include ../toolchain.mk

OUT   := $(BUILD)/libgui
LIB   := $(OUT)/libgui.a
SRCS  := $(wildcard src/*.c src/widgets/*.c)
OBJS  := $(patsubst %.c,$(OUT)/%.o,$(SRCS)) $(OUT)/font.o
UCPP  := -Iinclude -I../libfont/include -I../libwire/include -I../libwire/generated -I../libc/include -I../kernel/include

.PHONY: all clean check
all: $(LIB)

# Host unit tests: the library and libfont compiled with the system
# compiler against a fake client (tests/fake_client.c).
HOSTSRCS := $(filter-out src/client.c,$(SRCS)) $(wildcard ../libfont/src/*.c) $(wildcard tests/*.c)
check: $(OUT)/font.c
	@mkdir -p $(OUT)/host
	$(HOSTCC) -std=c17 -O1 -g -Wall -DMINIOS_HOST -Iinclude -I../libfont/include -Itests \
	    -o $(OUT)/host/test_framework $(HOSTSRCS) $(OUT)/font.c
	$(OUT)/host/test_framework

# The font is the kernel's, with the array renamed for user space.
$(OUT)/font.c: ../kernel/drivers/font.c
	@mkdir -p $(dir $@)
	sed -e 's|#include <drivers/font.h>|#include <gui/gfx.h>|' -e 's|font8x16\[256\]\[FONT_HEIGHT\]|gfx_font8x16[256][16]|' $< > $@

$(OUT)/font.o: $(OUT)/font.c
	$(CC) $(UCFLAGS) $(UCPP) -c -o $@ $<

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UCPP) -MMD -MP -c -o $@ $<

$(LIB): $(OBJS)
	rm -f $@
	$(AR) rcs $@ $(OBJS)

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)
