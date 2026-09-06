include ../toolchain.mk

OUT   := $(BUILD)/libc
LIB   := $(OUT)/libc.a
SO    := $(BUILD)/lib/libc.so
CRT0  := $(OUT)/crt0.o

SRCS  := $(wildcard src/*/*.c) $(wildcard src/*.c)
ASRCS := src/syscall.S src/setjmp.S
OBJS  := $(patsubst %.c,$(OUT)/%.o,$(SRCS)) $(patsubst %.S,$(OUT)/%.o,$(ASRCS))

.PHONY: all clean
all: $(LIB) $(SO) $(CRT0)

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -Iinclude -I../kernel/include -MMD -MP -c -o $@ $<

$(OUT)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(UASFLAGS) -Iinclude -I../kernel/include -c -o $@ $<

$(CRT0): src/crt0.S
	@mkdir -p $(dir $@)
	$(CC) $(UASFLAGS) -Iinclude -c -o $@ $<

# A thread local variable would give the library a TLS segment, which the
# loader does not support; the link fails when one appears.
$(SO): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(USOFLAGS) -soname libc.so -o $@ $(OBJS) $(LIBGCC)
	@if $(READELF) -l $@ | grep -q ' TLS '; then echo "libc.so has a TLS segment"; rm -f $@; exit 1; fi

$(LIB): $(OBJS)
	rm -f $@
	$(AR) rcs $@ $(OBJS)

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)
