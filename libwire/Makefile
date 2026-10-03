include ../toolchain.mk

OUT   := $(BUILD)/libwire
LIB   := $(OUT)/libwire.a
SO    := $(BUILD)/lib/libwire.so
# The ABI number of the shared object (docs/design/packages.md). It is
# raised whenever a structure, constant, signature or documented behaviour
# that programs depend on changes incompatibly, and additions leave it
# alone.
ABI   := 1
ABIFILE := $(BUILD)/lib/libwire.abi
SRCS  := $(wildcard src/*.c) $(wildcard generated/*.c)
OBJS  := $(patsubst %.c,$(OUT)/%.o,$(SRCS))
UCPP  := -Iinclude -Igenerated -I../libc/include -I../kernel/include

.PHONY: all clean check generate
all: $(LIB) $(SO) $(ABIFILE)

# Regenerate the marshalling code from the protocol files.
generate:
	for p in core shell seat data text debug audio ime; do python3 ../tools/wscan/wscan.py ../protocol/$$p.xml generated; done

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UCPP) -MMD -MP -c -o $@ $<

$(SO): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(USOFLAGS) -soname libwire.so -o $@ $(OBJS) -L$(BUILD)/lib -lc $(LIBGCC)

$(LIB): $(OBJS)
	rm -f $@
	$(AR) rcs $@ $(OBJS)

# Host unit tests over a socketpair.
check:
	@mkdir -p $(OUT)/host
	$(HOSTCC) $(HOSTCPPFLAGS) -std=c17 -O1 -g -Wall -DMINIOS_HOST -Iinclude -Igenerated -Itests \
	    -o $(OUT)/host/test_wire $(SRCS) tests/test_wire.c
	$(OUT)/host/test_wire

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)

$(ABIFILE): Makefile
	@mkdir -p $(dir $@)
	@echo $(ABI) > $@
