include ../toolchain.mk

OUT  := $(BUILD)/libaudio
LIB  := $(OUT)/libaudio.a
SO   := $(BUILD)/lib/libaudio.so
SRCS := $(wildcard src/*.c)
OBJS := $(patsubst %.c,$(OUT)/%.o,$(SRCS))
UCPP := -Iinclude -I../libwire/include -I../libwire/generated \
        -I../libc/include -I../kernel/include

.PHONY: all clean
all: $(LIB) $(SO)

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UCPP) -MMD -MP -c -o $@ $<

$(SO): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(USOFLAGS) -soname libaudio.so -o $@ $(OBJS) -L$(BUILD)/lib -lwire -lc $(LIBGCC)

$(LIB): $(OBJS)
	rm -f $@
	$(AR) rcs $@ $(OBJS)

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)
