include ../toolchain.mk

OUT   := $(BUILD)/libfont
LIB   := $(OUT)/libfont.a
SRCS  := $(wildcard src/*.c)
OBJS  := $(patsubst %.c,$(OUT)/%.o,$(SRCS))
UCPP  := -Iinclude -I../libc/include -I../kernel/include

.PHONY: all clean
all: $(LIB)

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UCPP) -MMD -MP -c -o $@ $<

$(LIB): $(OBJS)
	rm -f $@
	$(AR) rcs $@ $(OBJS)

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)
