#pragma once
/* Formats and codecs (docs/design/codecs.md). The library maintains a
 * registry of the codecs that the modules in /lib/codecs provide, in the
 * manner of the gdk-pixbuf loaders and the GStreamer plugins. A program
 * passes a file or the bytes of one, and the registry selects the codec
 * by the content of the data first and by the extension of the file name
 * second.
 *
 * An image is w by h pixels of 0xAARRGGBB with straight alpha in memory
 * allocated with malloc. Audio is interleaved signed 32 bit samples with
 * full scale at 2^31, whatever the sample size of the file. Functions
 * that can fail return a negative errno value, and lookup functions
 * return NULL when nothing matches. */
#include <stddef.h>
#include <stdint.h>

/* The version of the interface between the library and its modules. The
 * registry does not load a module built for a different version. Version
 * 2 appended audio_encode_options to struct codec. Version 3 appended the
 * functions of animations. */
#define CODEC_MODULE_ABI 3
/* The directory of the modules. The environment variable CODEC_PATH
 * overrides it. */
#define CODEC_DIR "/usr/lib/codecs"

enum codec_kind { CODEC_IMAGE = 1, CODEC_AUDIO = 2 };

#define CODEC_DECODE 1
#define CODEC_ENCODE 2
/* A vector format that renders at the size of the request, like
 * GDK_PIXBUF_FORMAT_SCALABLE. */
#define CODEC_SCALABLE 4
/* An image format with animations: the codec decodes the frames of an
 * animation one after another, and encodes animations when it has
 * CODEC_ENCODE as well. image_decode returns the first frame. */
#define CODEC_ANIMATED 8

struct codec_picture {
    int w, h;
    uint32_t *pixels;
};

/* Parameters for a decoder. A vector format renders at width by height
 * pixels (0 selects the size stored in the file) and fills shapes that
 * have no colour of their own with color (0x00rrggbb). Raster formats
 * ignore these parameters. */
struct codec_image_request {
    int width, height;
    uint32_t color;
};

/* An animation: every frame covers the whole canvas of w by h pixels.
 * frames is the number of frames, or -1 when it is unknown. loops is the
 * number of times the animation plays, 0 for a repetition without end. */
struct codec_animation_info {
    int w, h;
    int frames;
    int loops;
};

/* A frame for an encoder: w by h pixels of the canvas, and the time in
 * milliseconds that the frame is shown. */
struct codec_frame {
    const uint32_t *pixels;
    int delay_ms;
};

/* The format of an audio stream: frames per second, channels, and the
 * bits of a sample in the file. */
struct codec_audio_format {
    int rate, channels, bits;
};

/* One codec. probe rates the first bytes of some data from 0 (not this
 * format) to 100 (certainly this format) and receives at most
 * CODEC_PROBE_LEN bytes. The function pointers for a capability that the
 * codec lacks are NULL. */
#define CODEC_PROBE_LEN 512
struct codec {
    const char *name;               /* "png" */
    const char *description;        /* "Portable Network Graphics" */
    enum codec_kind kind;
    int caps;                       /* CODEC_DECODE, CODEC_ENCODE, CODEC_SCALABLE */
    const char *mime_types;         /* separated by spaces */
    const char *extensions;         /* lower case, separated by spaces */
    int (*probe)(const uint8_t *data, size_t len);
    /* Images. decode fills *out. encode stores the encoded file in memory
     * allocated with malloc at *data and returns its length. */
    int (*image_decode)(const uint8_t *data, size_t len, const struct codec_image_request *req,
                        struct codec_picture *out);
    long (*image_encode)(const struct codec_picture *pic, uint8_t **data);
    /* Audio. open checks the data, fills in the format and the number of
     * frames, and creates a decoder state. read decodes up to frames
     * frames from that state and returns 0 at the end of the stream. The
     * data must remain valid until close. encode writes frames frames of
     * samples in the given format. */
    int (*audio_open)(const uint8_t *data, size_t len, struct codec_audio_format *fmt, long *frames,
                      void **state);
    long (*audio_read)(void *state, int32_t *samples, long frames);
    void (*audio_close)(void *state);
    long (*audio_encode)(const struct codec_audio_format *fmt, const int32_t *samples, long frames,
                         uint8_t **data);
    /* Encode with options, a string of name=value pairs separated by
     * commas, or NULL. A codec without options leaves this NULL. */
    long (*audio_encode_options)(const struct codec_audio_format *fmt, const int32_t *samples, long frames,
                                 const char *options, uint8_t **data);
    /* Animations (CODEC_ANIMATED). open checks the data, fills in info and
     * creates a decoder state. next composes the next frame into pixels,
     * w by h values, stores its delay in milliseconds, and returns 1, 0
     * after the last frame, or a negative errno value. The data must
     * remain valid until close. encode writes count frames. */
    int (*animation_open)(const uint8_t *data, size_t len, struct codec_animation_info *info, void **state);
    int (*animation_next)(void *state, uint32_t *pixels, int *delay_ms);
    void (*animation_close)(void *state);
    long (*animation_encode)(const struct codec_animation_info *info, const struct codec_frame *frames, int count,
                             uint8_t **data);
};

/* The one symbol a module exports. */
struct codec_module {
    int abi;                        /* CODEC_MODULE_ABI */
    const char *name;
    int count;
    const struct codec *codecs;
};

/* A module defines its table with CODEC_MODULE(name). The table is the
 * only symbol that a module exports, because modules are compiled with
 * hidden visibility. When the modules are compiled into a program with
 * CODEC_BUILTIN (the host tests), each module has a symbol of its own
 * name, and the registry registers them instead of loading files. */
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
 * lookup. Returns 0, or -EINVAL when the ABI number does not match. */
int codec_register(const struct codec_module *m, const char *path);

/* Lookups. A kind of 0 matches both kinds. caps lists the capabilities
 * that the codec must have, and 0 accepts any codec. */
const struct codec *codec_find(const char *name);
const struct codec *codec_for_mime(enum codec_kind kind, const char *mime, int caps);
const struct codec *codec_for_path(enum codec_kind kind, const char *path, int caps);
const struct codec *codec_for_data(enum codec_kind kind, const uint8_t *data, size_t len, int caps);
/* The codec for some data, selected by content and, when no probe
 * matches, by the extension of path. path may be NULL. */
const struct codec *codec_identify(enum codec_kind kind, const uint8_t *data, size_t len, const char *path,
                                   int caps);

/* ---- files ---- */

/* Read a whole file into memory allocated with malloc, and write a whole
 * file. */
int codec_read_file(const char *path, uint8_t **data, size_t *len);
int codec_write_file(const char *path, const uint8_t *data, size_t len);

/* ---- images ---- */

/* Decode with c, or with the codec that codec_identify selects when c is
 * NULL. path and req may be NULL. Returns -ENOTSUP when no codec decodes
 * the data. */
int codec_image_decode(const struct codec *c, const uint8_t *data, size_t len, const char *path,
                       const struct codec_image_request *req, struct codec_picture *out);
int codec_image_load(const char *path, const struct codec_image_request *req, struct codec_picture *out);
long codec_image_encode(const struct codec *c, const struct codec_picture *pic, uint8_t **data);
/* Save with the codec called name, or with the codec for the extension
 * of path when name is NULL. */
int codec_image_save(const struct codec_picture *pic, const char *path, const char *name);
void codec_picture_free(struct codec_picture *pic);

/* ---- animations ---- */

struct codec_animation;
/* Open a decoder of the frames of data, with c or with the codec that
 * codec_identify selects among the animated codecs. The data must remain
 * valid until the decoder is closed. Returns -ENOTSUP when no animated
 * codec decodes the data. */
int codec_animation_open(const struct codec *c, const uint8_t *data, size_t len, const char *path,
                         struct codec_animation **out);
/* Open a file. The decoder stores the file contents in memory and
 * codec_animation_close frees them. */
int codec_animation_open_file(const char *path, struct codec_animation **out);
const struct codec_animation_info *codec_animation_info(const struct codec_animation *a);
const struct codec *codec_animation_codec(const struct codec_animation *a);
/* Compose the next frame into pixels (w * h values). Returns 1, 0 after
 * the last frame, or a negative errno value. */
int codec_animation_next(struct codec_animation *a, uint32_t *pixels, int *delay_ms);
/* Start again at the first frame, for an animation that repeats. */
int codec_animation_rewind(struct codec_animation *a);
void codec_animation_close(struct codec_animation *a);
long codec_animation_encode(const struct codec *c, const struct codec_animation_info *info,
                            const struct codec_frame *frames, int count, uint8_t **data);
/* Save with the codec called name, or with the codec for the extension
 * of path when name is NULL. */
int codec_animation_save(const char *path, const char *name, const struct codec_animation_info *info,
                         const struct codec_frame *frames, int count);

/* ---- audio ---- */

struct codec_audio;
/* Open a decoder for data, which must remain valid until the decoder is
 * closed. */
int codec_audio_open(const struct codec *c, const uint8_t *data, size_t len, const char *path,
                     struct codec_audio **out);
/* Open a file. The decoder stores the file contents in memory and
 * codec_audio_close frees them. */
int codec_audio_open_file(const char *path, struct codec_audio **out);
const struct codec_audio_format *codec_audio_format(const struct codec_audio *a);
long codec_audio_frames(const struct codec_audio *a);          /* -1 when unknown */
const struct codec *codec_audio_codec(const struct codec_audio *a);
/* Decode up to frames frames into samples (frames * channels values).
 * Returns the number of frames, or 0 at the end of the stream. */
long codec_audio_read(struct codec_audio *a, int32_t *samples, long frames);
void codec_audio_close(struct codec_audio *a);
long codec_audio_encode(const struct codec *c, const struct codec_audio_format *fmt, const int32_t *samples,
                        long frames, uint8_t **data);
int codec_audio_save(const char *path, const char *name, const struct codec_audio_format *fmt,
                     const int32_t *samples, long frames);
/* The same with encoder options, such as "quality=0.6". -EINVAL when the
 * codec takes no options or rejects one. */
long codec_audio_encode_options(const struct codec *c, const struct codec_audio_format *fmt, const int32_t *samples,
                                long frames, const char *options, uint8_t **data);
int codec_audio_save_options(const char *path, const char *name, const struct codec_audio_format *fmt,
                             const int32_t *samples, long frames, const char *options);
/* The value of option name in options, copied into value. Returns 1 when
 * the option is present, else 0. */
int codec_option(const char *options, const char *name, char *value, size_t size);
/* Returns 0 when every option in options is one of the names in known, a
 * list separated by spaces, else -EINVAL. */
int codec_options_check(const char *options, const char *known);

/* ---- shared helpers of the modules ---- */

/* Inflate a zlib stream (RFC 1950) into dst and return the output
 * length. */
long codec_inflate(uint8_t *dst, size_t cap, const uint8_t *src, size_t len);

/* Ogg (RFC 3533), the container of Vorbis, Opus and Ogg FLAC. The reader
 * follows one logical stream of a file in memory and returns its packets
 * in order. accept selects the stream by its first packet: the reader
 * starts with the first stream it accepts, skips the pages of other
 * streams in a multiplexed file, and after the end of the stream moves on
 * to the next chained stream that it accepts, if there is one. */
struct codec_ogg_packet {
    const uint8_t *data;
    size_t len;
    int64_t granule;                    /* of the page, for the last packet ending on it, else -1 */
    int bos;                            /* the first packet of a logical stream */
    int eos;                            /* the last packet of a logical stream */
    uint32_t serial;
};

struct codec_ogg_reader {
    const uint8_t *data;
    size_t len, next;                   /* the file, and the offset of the next page */
    int (*accept)(const uint8_t *packet, size_t len);
    uint32_t serial;
    int have_serial, ended;
    /* The current page of the stream: its lacing values and body. */
    const uint8_t *lacing, *body;
    unsigned segments, segment, last_end;
    size_t body_at;
    int64_t granule;
    int page_eos, page_bos;
    /* The packet being assembled across pages. */
    uint8_t *packet;
    size_t packet_len, packet_cap;
    int packet_bos;
};

void codec_ogg_reader_init(struct codec_ogg_reader *r, const uint8_t *data, size_t len,
                           int (*accept)(const uint8_t *packet, size_t len));
/* The next packet, valid until the next call. Returns 1, 0 at the end of
 * the last accepted stream, or -EBADMSG for a damaged page. */
int codec_ogg_next(struct codec_ogg_reader *r, struct codec_ogg_packet *p);
void codec_ogg_reader_free(struct codec_ogg_reader *r);
/* The first packet of the first logical stream in data that accept
 * accepts, for probes. Returns its length, or 0. */
size_t codec_ogg_first_packet(const uint8_t *data, size_t len, int (*accept)(const uint8_t *packet, size_t len),
                              const uint8_t **packet);
/* The probe score of an Ogg codec: 100 when the first logical stream of
 * the file is one that accept accepts, 90 when a later stream of a
 * multiplexed file is, else 0. The first stream decides the codec of a
 * multiplexed file. */
int codec_ogg_probe(const uint8_t *data, size_t len, int (*accept)(const uint8_t *packet, size_t len));
/* The granule position of the last page of every chained stream that
 * accept accepts, summed. Returns -1 when no such page exists. */
int64_t codec_ogg_total_granule(const uint8_t *data, size_t len, int (*accept)(const uint8_t *packet, size_t len));
uint32_t codec_ogg_crc(const uint8_t *p, size_t n);

/* The channel order of Vorbis I (section 4.3.9 of its specification),
 * which Opus mapping family 1 also uses. For each position in the WAV
 * channel order, the function stores the Vorbis channel to read from in
 * order[position]. The array order must have room for 8 entries. With more
 * than 8 channels, the order is the identity. */
void codec_vorbis_channel_order(int *order, unsigned channels);

/* The writer packs the packets of one logical stream into pages of about
 * 4 KiB. A page carries the granule position of the last packet completed
 * on it. codec_ogg_flush ends the current page, for example after the
 * first header packet, which must be alone on the first page, and with
 * eos set it writes the last page of the stream. The pages accumulate in
 * out[0..len). */
struct codec_ogg_writer {
    uint32_t serial, sequence;
    uint8_t *out;
    size_t len, cap;
    uint8_t lacing[255];
    unsigned segments;
    uint8_t *body;
    size_t body_len, body_cap;
    int64_t granule;
    int first, continued, failed;
};

void codec_ogg_writer_init(struct codec_ogg_writer *w, uint32_t serial);
int codec_ogg_write_packet(struct codec_ogg_writer *w, const uint8_t *data, size_t len, int64_t granule);
int codec_ogg_flush(struct codec_ogg_writer *w, int eos);
void codec_ogg_writer_free(struct codec_ogg_writer *w);

/* MD5 (RFC 1321), used by FLAC for the checksum of the audio data. */
struct codec_md5 {
    uint32_t h[4];
    uint64_t len;
    uint8_t buf[64];
};
void codec_md5_init(struct codec_md5 *m);
void codec_md5_update(struct codec_md5 *m, const void *data, size_t len);
void codec_md5_final(struct codec_md5 *m, uint8_t digest[16]);
