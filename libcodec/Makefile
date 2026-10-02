include ../toolchain.mk

OUT   := $(BUILD)/libcodec
LIB   := $(OUT)/libcodec.a
SO    := $(BUILD)/lib/libcodec.so
# The ABI number of the shared object (docs/design/packages.md): raised
# whenever a structure, constant, signature or documented behaviour that
# programs depend on changes incompatibly; additions leave it alone. The
# interface of the modules has its own number, CODEC_MODULE_ABI.
ABI   := 1
ABIFILE := $(BUILD)/lib/libcodec.abi
SRCS  := $(wildcard src/*.c)
OBJS  := $(patsubst %.c,$(OUT)/%.o,$(SRCS))
UCPP  := -Iinclude -I../libc/include -I../kernel/include
# Every directory of modules/ is one module: modules/NAME/*.c become
# $(BUILD)/lib/codecs/NAME.so, installed in /lib/codecs.
MODULES := $(notdir $(wildcard modules/*))
MODSOS  := $(addprefix $(BUILD)/lib/codecs/,$(addsuffix .so,$(MODULES)))
MODOBJS := $(patsubst %.c,$(OUT)/%.o,$(wildcard modules/*/*.c))

.PHONY: all clean
# The module objects are named here so that make does not treat them as
# intermediate files of the chain to the modules, which it would not
# remake when they are missing.
all: $(LIB) $(SO) $(ABIFILE) $(MODOBJS) $(MODSOS)

# A module exports only its codec_module. This rule comes first, since
# make 3.81 takes the first pattern that matches.
$(OUT)/modules/%.o: modules/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -fvisibility=hidden $(UCPP) -MMD -MP -c -o $@ $<

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UCPP) -MMD -MP -c -o $@ $<

$(SO): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(USOFLAGS) -soname libcodec.so -o $@ $(OBJS) -L$(BUILD)/lib -lc $(LIBGCC)

$(LIB): $(OBJS)
	rm -f $@
	$(AR) rcs $@ $(OBJS)

# The objects of module $(1); a function, because a % written in the
# rule below would be replaced by the stem before the second expansion.
module_objs = $(patsubst %.c,$(OUT)/%.o,$(wildcard modules/$(1)/*.c))
.SECONDEXPANSION:
$(BUILD)/lib/codecs/%.so: $$(call module_objs,$$*) $(SO)
	@mkdir -p $(dir $@)
	$(LD) $(USOFLAGS) -soname $*.so -o $@ $(filter %.o,$^) -L$(BUILD)/lib -lcodec -lc $(LIBGCC)

clean:
	rm -rf $(OUT) $(SO) $(ABIFILE) $(BUILD)/lib/codecs

-include $(OBJS:.o=.d) $(MODOBJS:.o=.d)

$(ABIFILE): Makefile
	@mkdir -p $(dir $@)
	@echo $(ABI) > $@
