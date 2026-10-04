include ../../toolchain.mk

# JSON output (docs/design/json.md): the string escaper and the streaming
# writer, for the programs that write JSON. It is a library of its own, so
# that the packages of those programs share it.
OUT   := $(BUILD)/libjson
LIB   := $(OUT)/libjson.a
SO    := $(BUILD)/lib/libjson.so
# The ABI number of the shared object (docs/design/packages.md). It is
# raised whenever a structure, constant, signature or documented behaviour
# that programs depend on changes incompatibly, and additions leave it
# alone.
ABI   := 1
ABIFILE := $(BUILD)/lib/libjson.abi
SRCS  := $(wildcard src/*.c)
OBJS  := $(patsubst %.c,$(OUT)/%.o,$(SRCS))
UCPP  := -Iinclude -I../libc/include -I../../kernel/include

.PHONY: all clean
all: $(LIB) $(SO) $(ABIFILE)

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UCPP) -MMD -MP -c -o $@ $<

$(SO): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(USOFLAGS) -soname libjson.so -o $@ $(OBJS) -L$(BUILD)/lib -lc $(LIBGCC)

$(LIB): $(OBJS)
	rm -f $@
	$(AR) rcs $@ $(OBJS)

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)

$(ABIFILE): Makefile
	@mkdir -p $(dir $@)
	@echo $(ABI) > $@
