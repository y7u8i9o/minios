#pragma once
/* Formats and codecs (docs/design/codecs.md): a registry of the codecs
 * that the modules in /lib/codecs provide, in the manner of the
 * gdk-pixbuf loaders and the GStreamer plugins. A program asks for an
 * image or an audio stream and the registry picks the codec by the
 * content of the data, then by the extension of the file name.
 *
 * Images are w by h pixels of 0xAARRGGBB with straight alpha, allocated
 * with malloc. Audio is interleaved signed 32 bit samples, full scale at
 * 2^31, whatever the sample size of the file. Functions that can fail
 * return a negative errno value; lookups return NULL. */
#include <stddef.h>
#include <stdint.h>

/* The interface between the library and its modules. A module built for
 * another number is not loaded. */
#define CODEC_MODULE_ABI 1
/* The directory of the modules; the environment variable CODEC_PATH
 * names another one. */
#define CODEC_DIR "/lib/codecs"

enum codec_kind { CODEC_IMAGE = 1, CODEC_AUDIO = 2 };

#define CODEC_DECODE 1
#define CODEC_ENCODE 2

struct codec_picture {
    int w, h;
    uint32_t *pixels;
};

/* What the caller would like from a decoder: vector formats render at
 * width by height pixels (0: their own size) and fill shapes without a
 * colour of their own with color (0x00rrggbb); raster formats ignore
 * both. */
struct codec_image_request {
    int width, height;
    uint32_t color;
};

/* The format of an audio stream: frames per second, channels, and the
 * bits of a sample in the file. */
struct codec_audio_format {
    int rate, channels, bits;
};

/* One codec. probe scores the first bytes of some data from 0 (not this
 * format) to 100 (certainly this format); it sees at most CODEC_PROBE_LEN
 * bytes. The functions of a capability the codec lacks are NULL. */
#define CODEC_PROBE_LEN 512
struct codec {
    const char *name;               /* "png" */
    const char *description;        /* "Portable Network Graphics" */
    enum codec_kind kind;
    int caps;                       /* CODEC_DECODE, CODEC_ENCODE */
    const char *mime_types;         /* separated by spaces */
    const char *extensions;         /* lower case, separated by spaces */
    int (*probe)(const uint8_t *data, size_t len);
    /* Images: decode fills *out; encode stores a malloc'ed file in *data
     * and returns its length. */
    int (*image_decode)(const uint8_t *data, size_t len, const struct codec_image_request *req,
                        struct codec_picture *out);
    long (*image_encode)(const struct codec_picture *pic, uint8_t **data);
    /* Audio: open checks the data, fills the format and the number of
     * frames, and returns a decoder state that read takes up to frames
     * frames from (0 at the end); the data must stay valid until close.
     * encode writes frames frames of samples in the format. */
    int (*audio_open)(const uint8_t *data, size_t len, struct codec_audio_format *fmt, long *frames,
                      void **state);
    long (*audio_read)(void *state, int32_t *samples, long frames);
    void (*audio_close)(void *state);
    long (*audio_encode)(const struct codec_audio_format *fmt, const int32_t *samples, long frames,
                         uint8_t **data);
};

/* The one symbol a module exports. */
struct codec_module {
    int abi;                        /* CODEC_MODULE_ABI */
    const char *name;
    int count;
    const struct codec *codecs;
};

/* A module defines its table with CODEC_MODULE(name); it is the only
 * symbol a module exports, since modules are compiled with hidden
 * visibility. Compiled into a program with CODEC_BUILTIN (the host
 * tests), each module has its own symbol and the registry lists them
 * instead of loading files. */
#ifdef CODEC_BUILTIN
#define CODEC_MODULE(name) const struct codec_module codec_module_##name
#else
#define CODEC_MODULE(name) __attribute__((visibility("default"))) const struct codec_module codec_module
#endif

/* ---- the registry ---- */

int codec_count(void);
const struct codec *codec_get(int index);
int codec_module_count(void);
const struct codec_module *codec_module_get(int index);
const char *codec_module_path(int index);       /* the file it was loaded from */
/* Add a module that is not in the directory, before or after the first
 * lookup. Returns 0, or -EINVAL for a wrong ABI number. */
int codec_register(const struct codec_module *m, const char *path);

/* Lookups. kind 0 matches both kinds; caps is the capabilities the codec
 * must have (0 for any). */
const struct codec *codec_find(const char *name);
const struct codec *codec_for_mime(enum codec_kind kind, const char *mime, int caps);
const struct codec *codec_for_path(enum codec_kind kind, const char *path, int caps);
const struct codec *codec_for_data(enum codec_kind kind, const uint8_t *data, size_t len, int caps);
/* The codec for some data: by content, else by the extension of path
 * (which may be NULL). */
const struct codec *codec_identify(enum codec_kind kind, const uint8_t *data, size_t len, const char *path,
                                   int caps);

/* ---- files ---- */

/* Read a whole file into a malloc'ed buffer; write one. */
int codec_read_file(const char *path, uint8_t **data, size_t *len);
int codec_write_file(const char *path, const uint8_t *data, size_t len);

/* ---- images ---- */

/* Decode with the codec codec_identify picks (path may be NULL), or with
 * c when it is not NULL. req may be NULL. -ENOTSUP: no codec decodes the
 * data. */
int codec_image_decode(const struct codec *c, const uint8_t *data, size_t len, const char *path,
                       const struct codec_image_request *req, struct codec_picture *out);
int codec_image_load(const char *path, const struct codec_image_request *req, struct codec_picture *out);
long codec_image_encode(const struct codec *c, const struct codec_picture *pic, uint8_t **data);
/* Save in the format named by name, or by the extension of path when
 * name is NULL. */
int codec_image_save(const struct codec_picture *pic, const char *path, const char *name);
void codec_picture_free(struct codec_picture *pic);

/* ---- audio ---- */

struct codec_audio;
/* Open a decoder for data, which must stay valid until the decoder is
 * closed. */
int codec_audio_open(const struct codec *c, const uint8_t *data, size_t len, const char *path,
                     struct codec_audio **out);
/* Open a file; the decoder owns the file's contents. */
int codec_audio_open_file(const char *path, struct codec_audio **out);
const struct codec_audio_format *codec_audio_format(const struct codec_audio *a);
long codec_audio_frames(const struct codec_audio *a);          /* -1 when unknown */
const struct codec *codec_audio_codec(const struct codec_audio *a);
/* Up to frames frames into samples (frames * channels values); 0 at the
 * end. */
long codec_audio_read(struct codec_audio *a, int32_t *samples, long frames);
void codec_audio_close(struct codec_audio *a);
long codec_audio_encode(const struct codec *c, const struct codec_audio_format *fmt, const int32_t *samples,
                        long frames, uint8_t **data);
int codec_audio_save(const char *path, const char *name, const struct codec_audio_format *fmt,
                     const int32_t *samples, long frames);

/* ---- shared helpers of the modules ---- */

/* A zlib stream (RFC 1950) into dst; returns the output length. */
long codec_inflate(uint8_t *dst, size_t cap, const uint8_t *src, size_t len);
