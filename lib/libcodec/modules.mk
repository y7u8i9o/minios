# Third-party sources of the codec modules. MODULE_SRCS_NAME lists the
# sources of module NAME, and MODULE_CPP_NAME contains the preprocessor
# flags that those sources and the module's own files both need. The build
# of libcodec places their objects under $(OUT)/third_party. The host tests
# of libcodec and libgui include this file as well; its paths are relative
# to lib/libcodec and lib/libgui, which are at the same depth. The
# top-level Makefile sets OPUS_DIR to the path from the top directory
# before it includes this file.
OPUS_DIR ?= ../../third_party/opus
MODULE_SRCS_opus := $(wildcard $(OPUS_DIR)/celt/*.c $(OPUS_DIR)/silk/*.c $(OPUS_DIR)/src/*.c)
MODULE_CPP_opus  := -DOPUS_BUILD -DVAR_ARRAYS -I$(OPUS_DIR)/include -I$(OPUS_DIR)/celt -I$(OPUS_DIR)/silk
