include ../toolchain.mk

OUT   := $(BUILD)/libwire
LIB   := $(OUT)/libwire.a
SRCS  := $(wildcard src/*.c) $(wildcard generated/*.c)
OBJS  := $(patsubst %.c,$(OUT)/%.o,$(SRCS))
UCPP  := -Iinclude -Igenerated -I../libc/include -I../kernel/include

.PHONY: all clean check generate
all: $(LIB)

# Regenerate the marshalling code from the protocol files.
generate:
	for p in core shell seat data text debug; do python3 ../tools/wscan/wscan.py ../protocol/$$p.xml generated; done

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UCPP) -MMD -MP -c -o $@ $<

$(LIB): $(OBJS)
	rm -f $@
	$(AR) rcs $@ $(OBJS)

# Host unit tests over a socketpair.
check:
	@mkdir -p $(OUT)/host
	$(HOSTCC) -std=c17 -O1 -g -Wall -DMINIOS_HOST -Iinclude -Igenerated -Itests \
	    -o $(OUT)/host/test_wire $(SRCS) tests/test_wire.c
	$(OUT)/host/test_wire

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)
