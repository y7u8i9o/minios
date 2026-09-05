include ../toolchain.mk

OUT := $(BUILD)/libedit
LIB := $(OUT)/libedit.a
SRCS := $(wildcard src/*.c)
OBJS := $(patsubst %.c,$(OUT)/%.o,$(SRCS))

.PHONY: all clean
all: $(LIB)

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -Iinclude -I../libc/include -I../kernel/include -MMD -MP -c -o $@ $<

$(LIB): $(OBJS)
	$(AR) rcs $@ $(OBJS)

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)
