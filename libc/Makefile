include ../toolchain.mk

OUT   := $(BUILD)/libc
LIB   := $(OUT)/libc.a
SO    := $(BUILD)/lib/libc.so
# The ABI number of the shared object (docs/design/packages.md). It is
# raised whenever a structure, constant, signature or documented behaviour
# that programs depend on changes incompatibly, and additions leave it
# alone.
# 2 since the profiler client moved to libprof (docs/plan/packaging.md, P2).
ABI   := 2
ABIFILE := $(BUILD)/lib/libc.abi
CRT0  := $(OUT)/crt0.o
CRTI  := $(OUT)/crti.o $(OUT)/crtn.o
# The code of the target architecture comprises the system call stub, the
# start files, setjmp, the floating point environment and the math
# primitives (docs/design/libc.md).
ARCHDIR := arch/$(ARCH)

SRCS  := $(wildcard src/*/*.c) $(wildcard src/*.c) $(wildcard $(ARCHDIR)/*.c)
ASRCS := $(ARCHDIR)/syscall.S $(ARCHDIR)/setjmp.S
LIBC_CPPFLAGS := -Iinclude -I$(ARCHDIR) -Isrc -I../kernel/include
OBJS  := $(patsubst %.c,$(OUT)/%.o,$(SRCS)) $(patsubst %.S,$(OUT)/%.o,$(ASRCS))

.PHONY: all clean
all: $(LIB) $(SO) $(CRT0) $(CRTI) $(ABIFILE)

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(LIBC_CPPFLAGS) -MMD -MP -c -o $@ $<

$(OUT)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(UASFLAGS) $(LIBC_CPPFLAGS) -c -o $@ $<

$(CRT0): $(ARCHDIR)/crt0.S
	@mkdir -p $(dir $@)
	$(CC) $(UASFLAGS) -Iinclude -c -o $@ $<

$(OUT)/crti.o: $(ARCHDIR)/crti.S
	$(CC) $(UASFLAGS) -c -o $@ $<

$(OUT)/crtn.o: $(ARCHDIR)/crtn.S
	$(CC) $(UASFLAGS) -c -o $@ $<

$(SO): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(USOFLAGS) -soname libc.so -o $@ $(OBJS) $(LIBGCC)

$(LIB): $(OBJS)
	rm -f $@
	$(AR) rcs $@ $(OBJS)

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)

$(ABIFILE): Makefile
	@mkdir -p $(dir $@)
	@echo $(ABI) > $@
