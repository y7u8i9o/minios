include ../toolchain.mk

OUT   := $(BUILD)/libfont
LIB   := $(OUT)/libfont.a
SRCS  := $(wildcard src/*.c)
OBJS  := $(patsubst %.c,$(OUT)/%.o,$(SRCS))
UCPP  := -Iinclude -I../libc/include -I../kernel/include

.PHONY: all clean check
all: $(LIB)

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UCPP) -MMD -MP -c -o $@ $<

$(LIB): $(OBJS)
	rm -f $@
	$(AR) rcs $@ $(OBJS)

# Host test of the rasterizer against the bundled DejaVu font.
check: tests/test_raster.c $(SRCS)
	@mkdir -p $(OUT)/host
	$(HOSTCC) -std=c11 -O1 -Wall -Iinclude -o $(OUT)/host/test_raster tests/test_raster.c $(SRCS)
	$(OUT)/host/test_raster ../third_party/dejavu/DejaVuSans.ttf

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)
