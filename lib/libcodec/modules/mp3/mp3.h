#pragma once
/* The functions of mp3.so that decode.c and encode.c share. */
#include <codec/codec.h>

/* The decoder of minimp3 skips 528 + 1 samples at the start of a stream
 * with an Info tag in addition to the delay of the encoder. The LAME
 * extension of the tag stores the delay and the padding relative to that
 * number. */
#define MP3_DECODER_DELAY 529

long mp3_encode(const struct codec_audio_format *fmt, const int32_t *samples, long frames, uint8_t **data);
long mp3_encode_options(const struct codec_audio_format *fmt, const int32_t *samples, long frames,
                        const char *options, uint8_t **data);
