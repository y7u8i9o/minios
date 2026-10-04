#pragma once
/* The PNG module: decode.c, encode.c and the codec table in module.c. */
#include <codec/codec.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const uint8_t png_signature[8];
int png_decode(const uint8_t *data, size_t len, const struct codec_image_request *req, struct codec_picture *out);
long png_encode(const struct codec_picture *pic, uint8_t **data);
