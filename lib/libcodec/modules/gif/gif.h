#pragma once
/* The functions of gif.so that decode.c and encode.c share. */
#include <codec/codec.h>

int gif_probe(const uint8_t *data, size_t len);
int gif_image_decode(const uint8_t *data, size_t len, const struct codec_image_request *req,
                     struct codec_picture *out);
long gif_image_encode(const struct codec_picture *pic, uint8_t **data);
int gif_animation_open(const uint8_t *data, size_t len, struct codec_animation_info *info, void **state);
int gif_animation_next(void *state, uint32_t *pixels, int *delay_ms);
void gif_animation_close(void *state);
long gif_animation_encode(const struct codec_animation_info *info, const struct codec_frame *frames, int count,
                          uint8_t **data);
