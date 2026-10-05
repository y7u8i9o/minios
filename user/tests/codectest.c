/* Tests of the codec library and its modules in /lib/codecs
 * (docs/design/codecs.md). "codectest image" checks the registry, probing
 * and lookups, image round trips through the modules and through libgui,
 * the errors, and CODEC_PATH. "codectest audio" checks the audio streams
 * and the WAV module, "codectest flac" the FLAC module and "codectest
 * vorbis" the Vorbis decoder, both with the fixtures of
 * tools/gen_codec_fixtures.py, "codectest vorbisenc" the Vorbis encoder,
 * "codectest oggflac" FLAC in Ogg, "codectest mp3" the MP3 decoder and
 * encoder and "codectest gif" the GIF decoder and encoder with
 * animations. Any other argument runs every section. "codectest count" prints the number of codecs and is used by
 * the child process that runs with a different CODEC_PATH. */
#include <codec/codec.h>
#include <gui/image.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("codectest: FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static const char svg_text[] =
    "<?xml version=\"1.0\"?>\n<!-- a square -->\n"
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 16 16\"><path d=\"M4 4H12V12H4Z\"/></svg>";

static void test_registry(void)
{
    const struct codec *png = codec_find("png"), *svg = codec_find("SVG");
    CHECK(png && png->kind == CODEC_IMAGE && png->caps == (CODEC_DECODE | CODEC_ENCODE), "png codec");
    CHECK(svg && svg->kind == CODEC_IMAGE && svg->caps == (CODEC_DECODE | CODEC_SCALABLE),
          "svg codec, found without regard to case");
    CHECK(png && !(png->caps & CODEC_SCALABLE), "png is not scalable");
    CHECK(codec_find("nothing") == NULL, "unknown name");
    CHECK(codec_for_mime(CODEC_IMAGE, "image/svg+xml", CODEC_DECODE) == svg, "svg by MIME type");
    CHECK(codec_for_mime(CODEC_AUDIO, "image/png", 0) == NULL, "kind filters MIME lookups");
    CHECK(codec_for_path(CODEC_IMAGE, "/home/Shot.PNG", CODEC_ENCODE) == png, "png by extension");
    CHECK(codec_for_path(CODEC_IMAGE, "/x/icon.svg", CODEC_ENCODE) == NULL, "svg cannot encode");
    CHECK(codec_for_path(CODEC_IMAGE, "/dir.png/file", 0) == NULL, "a dot in a directory is no extension");
    struct codec_module bad = { CODEC_MODULE_ABI + 1, "bad", 0, NULL };
    CHECK(codec_register(&bad, "test") == -EINVAL, "a module of another ABI is refused");
}

static void test_probe(void)
{
    static const uint8_t png_head[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    static const char text[] = "plain text, no image";
    const struct codec *png = codec_find("png"), *svg = codec_find("svg");
    CHECK(codec_for_data(0, png_head, sizeof png_head, 0) == png, "png by its signature");
    CHECK(codec_for_data(CODEC_IMAGE, (const uint8_t *)svg_text, sizeof svg_text - 1, 0) == svg,
          "svg after a declaration and a comment");
    CHECK(codec_for_data(0, (const uint8_t *)text, sizeof text - 1, 0) == NULL, "text matches no codec");
    CHECK(codec_identify(CODEC_IMAGE, (const uint8_t *)text, sizeof text - 1, "a.png", 0) == png,
          "the extension decides when the content does not");
    CHECK(codec_identify(CODEC_IMAGE, png_head, sizeof png_head, "a.svg", 0) == png, "the content wins");
}

static void test_images(void)
{
    /* Opaque, translucent and transparent pixels. */
    uint32_t px[6] = { 0xffff0000, 0xff00ff00, 0x800000ff, 0x00000000, 0xff123456, 0x40ffffff };
    struct codec_picture pic = { 3, 2, px }, back;
    uint8_t *data;
    long n = codec_image_encode(codec_find("png"), &pic, &data);
    CHECK(n > 8, "png encode: %ld", n);
    if (n <= 8)
        return;
    int err = codec_image_decode(NULL, data, (size_t)n, NULL, NULL, &back);
    CHECK(err == 0 && back.w == 3 && back.h == 2 && memcmp(back.pixels, px, sizeof px) == 0, "png round trip: %d", err);
    codec_picture_free(&back);
    CHECK(codec_image_decode(NULL, data, 20, NULL, NULL, &back) == -EINVAL, "a truncated png is invalid");
    free(data);

    /* JPEG is lossy and has no alpha: a flat colour returns within a few
     * steps per channel, and opaque. */
    uint32_t flat[64];
    for (int i = 0; i < 64; i++)
        flat[i] = 0xff3080c0;
    struct codec_picture square = { 8, 8, flat };
    n = codec_image_encode(codec_find("jpeg"), &square, &data);
    CHECK(n > 100, "jpeg encode: %ld", n);
    if (n > 100) {
        err = codec_image_decode(NULL, data, (size_t)n, NULL, NULL, &back);
        int close = err == 0 && back.w == 8 && back.h == 8;
        for (int i = 0; close && i < 64; i++) {
            uint32_t v = back.pixels[i];
            int dr = (int)(v >> 16 & 0xff) - 0x30, dg = (int)(v >> 8 & 0xff) - 0x80, db = (int)(v & 0xff) - 0xc0;
            close = v >> 24 == 0xff && dr * dr < 16 && dg * dg < 16 && db * db < 16;
        }
        CHECK(close, "jpeg round trip: %d", err);
        if (err == 0)
            codec_picture_free(&back);
        CHECK(codec_image_decode(NULL, data, 40, NULL, NULL, &back) == -EINVAL, "a truncated jpeg is invalid");
        free(data);
    }

    mkdir("/tmp", 0755);
    CHECK(codec_image_save(&pic, "/tmp/codec.png", NULL) == 0, "save by extension");
    err = codec_image_load("/tmp/codec.png", NULL, &back);
    CHECK(err == 0 && memcmp(back.pixels, px, sizeof px) == 0, "load: %d", err);
    codec_picture_free(&back);
    CHECK(codec_image_save(&pic, "/tmp/codec.xyz", NULL) == -ENOTSUP, "no codec for .xyz");
    CHECK(codec_image_save(&pic, "/tmp/codec.svg", NULL) == -ENOTSUP, "svg has no encoder");
    CHECK(codec_image_load("/tmp/missing.png", NULL, &back) == -ENOENT, "a missing file");
    static const char junk[] = "not an image at all";
    CHECK(codec_image_decode(NULL, (const uint8_t *)junk, sizeof junk, NULL, NULL, &back) == -ENOTSUP,
          "unknown data");

    struct codec_image_request req = { 16, 16, 0x00ff8000 };
    err = codec_image_decode(NULL, (const uint8_t *)svg_text, sizeof svg_text - 1, NULL, &req, &back);
    CHECK(err == 0 && back.w == 16 && back.h == 16, "svg render: %d", err);
    if (!err) {
        CHECK(back.pixels[8 * 16 + 8] == 0xffff8000, "svg fill colour: %08x", back.pixels[8 * 16 + 8]);
        CHECK(back.pixels[1 * 16 + 1] >> 24 == 0, "svg outside the path: %08x", back.pixels[16 + 1]);
    }
    codec_picture_free(&back);
    err = codec_image_decode(NULL, (const uint8_t *)svg_text, sizeof svg_text - 1, NULL, NULL, &back);
    CHECK(err == 0 && back.w == 256, "svg without a size: %d %d", err, back.w);
    codec_picture_free(&back);

    /* libgui's functions are the same codecs. */
    struct image *img = image_load("/tmp/codec.png");
    CHECK(img && img->w == 3 && memcmp(img->pixels, px, sizeof px) == 0, "image_load");
    image_free(img);
    img = image_render_svg(svg_text, sizeof svg_text - 1, 8, 0);
    CHECK(img && img->w == 8 && img->pixels[4 * 8 + 4] == 0xff000000, "image_render_svg");
    image_free(img);
    errno = 0;
    CHECK(image_decode((const uint8_t *)junk, sizeof junk) == NULL && errno == ENOTSUP, "image_decode errno %d", errno);
}

/* A child with CODEC_PATH naming an empty directory finds no codecs. */
static void test_path(void)
{
    mkdir("/tmp/nocodecs", 0755);
    int fd[2];
    if (pipe(fd) < 0)
        return;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fd[1], 1);
        close(fd[0]);
        char *const argv[] = { "codectest", "count", NULL };
        char *const envp[] = { "CODEC_PATH=/tmp/nocodecs", NULL };
        execve("/bin/codectest", argv, envp);
        _exit(127);
    }
    close(fd[1]);
    char out[32] = "";
    ssize_t k = read(fd[0], out, sizeof out - 1);
    close(fd[0]);
    int status;
    waitpid(pid, &status, 0);
    CHECK(k > 0 && strcmp(out, "0\n") == 0 && status == 0, "CODEC_PATH: '%s', status %d", out, status);
}

/* ---- audio ---- */

/* A test signal at a sample size: every value a multiple of the step of
 * that size, so that it survives the file unchanged. */
static int32_t signal_at(long i, int c, int bits)
{
    uint32_t v = (uint32_t)(i * 2654435761u + (uint32_t)c * 40503u);
    if (i == 0)
        v = 0x80000000u;                /* the most negative sample */
    else if (i == 1)
        v = 0x7fffffffu;
    uint32_t mask = bits == 32 ? 0xffffffffu : ~((1u << (32 - bits)) - 1);
    return (int32_t)(v & mask);
}

static void test_wav_round_trip(int bits, int channels)
{
    enum { FRAMES = 301 };
    int32_t *in = malloc(sizeof(int32_t) * FRAMES * channels), *out = malloc(sizeof(int32_t) * FRAMES * channels);
    if (!in || !out)
        return;
    for (long i = 0; i < FRAMES; i++)
        for (int c = 0; c < channels; c++)
            in[i * channels + c] = signal_at(i, c, bits);
    struct codec_audio_format fmt = { 22050, channels, bits };
    uint8_t *file;
    long n = codec_audio_encode(codec_find("wav"), &fmt, in, FRAMES, &file);
    long want = 44 + (long)FRAMES * channels * (bits / 8);
    CHECK(n == want + (want & 1), "wav %d bit %d channels: %ld bytes", bits, channels, n);
    struct codec_audio *a = NULL;
    int err = n > 0 ? codec_audio_open(NULL, file, (size_t)n, NULL, &a) : -1;
    CHECK(err == 0, "open %d bit: %d", bits, err);
    if (err == 0) {
        const struct codec_audio_format *f = codec_audio_format(a);
        CHECK(f->rate == 22050 && f->channels == channels && f->bits == bits && codec_audio_frames(a) == FRAMES,
              "format %d %d %d %ld", f->rate, f->channels, f->bits, codec_audio_frames(a));
        /* Odd chunks, to cross every boundary. */
        long got = 0, k;
        while ((k = codec_audio_read(a, out + got * channels, 7)) > 0)
            got += k;
        CHECK(got == FRAMES && memcmp(in, out, sizeof(int32_t) * FRAMES * channels) == 0,
              "%d bit %d channel samples: %ld frames", bits, channels, got);
        CHECK(codec_audio_read(a, out, 7) == 0, "read at the end");
        codec_audio_close(a);
    }
    if (n > 0)
        free(file);
    free(in);
    free(out);
}

static void put32le(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void test_audio(void)
{
    const struct codec *wav = codec_find("wav");
    CHECK(wav && wav->kind == CODEC_AUDIO && wav->caps == (CODEC_DECODE | CODEC_ENCODE), "wav codec");
    CHECK(codec_for_mime(CODEC_AUDIO, "audio/x-wav", CODEC_DECODE) == wav, "wav by MIME type");
    CHECK(codec_for_path(CODEC_AUDIO, "/home/Chime.WAV", 0) == wav, "wav by extension");
    CHECK(codec_for_path(CODEC_IMAGE, "/home/chime.wav", 0) == NULL, "the kind filters extensions");
    for (int bits = 8; bits <= 32; bits += 8)
        for (int ch = 1; ch <= 3; ch++)
            test_wav_round_trip(bits, ch);

    /* Eight bit samples are unsigned: 0x80 is silence. */
    int32_t s8[3] = { 0, 0x7f000000, (int32_t)0x80000000u };
    struct codec_audio_format mono8 = { 8000, 1, 8 };
    uint8_t *file;
    long n = codec_audio_encode(wav, &mono8, s8, 3, &file);
    CHECK(n == 48 && file[44] == 0x80 && file[45] == 0xff && file[46] == 0x00 && file[47] == 0, "8 bit bytes and the pad byte");
    if (n > 0) {
        CHECK(codec_for_data(0, file, (size_t)n, 0) == wav, "wav by content");
        CHECK(codec_for_data(CODEC_IMAGE, file, (size_t)n, 0) == NULL, "not as an image");
        struct codec_audio *a;
        CHECK(codec_audio_open(NULL, file, 12, NULL, &a) == -EINVAL, "a header alone is invalid");
        /* A cut in the middle of the last sample retains the whole frames. */
        file[40] = 3;
        put32le(file + 4, 0);
        if (codec_audio_open(NULL, file, 46, NULL, &a) == 0) {
            CHECK(codec_audio_frames(a) == 2, "frames of a truncated file: %ld", codec_audio_frames(a));
            codec_audio_close(a);
        } else {
            CHECK(0, "a truncated file opens");
        }
        free(file);
    }
    struct codec_audio_format odd = { 8000, 1, 12 };
    CHECK(codec_audio_encode(wav, &odd, s8, 3, &file) == -EINVAL, "12 bit samples are refused");

    /* The extensible format with the PCM sub-format, and with float. */
    uint8_t ext[68 + 4] = { 0 };
    memcpy(ext, "RIFF", 4);
    put32le(ext + 4, sizeof ext - 8);
    memcpy(ext + 8, "WAVEfmt ", 8);
    put32le(ext + 16, 40);
    ext[20] = 0xfe; ext[21] = 0xff;     /* WAVE_FORMAT_EXTENSIBLE */
    ext[22] = 2;                        /* channels */
    put32le(ext + 24, 44100);
    put32le(ext + 28, 44100 * 4);
    ext[32] = 4;
    ext[34] = 16;
    ext[36] = 22;                       /* cbSize */
    ext[38] = 16;                       /* valid bits */
    ext[44] = 1;                        /* sub-format PCM */
    memcpy(ext + 60, "data", 4);
    put32le(ext + 64, 4);
    ext[68] = 0x00; ext[69] = 0x40; ext[70] = 0x00; ext[71] = 0xc0;
    struct codec_audio *a;
    int err = codec_audio_open(NULL, ext, sizeof ext, NULL, &a);
    CHECK(err == 0, "extensible PCM: %d", err);
    if (!err) {
        int32_t frame[2];
        CHECK(codec_audio_read(a, frame, 1) == 1 && frame[0] == 0x40000000 && frame[1] == (int32_t)0xc0000000u,
              "extensible samples %08x %08x", (unsigned)frame[0], (unsigned)frame[1]);
        codec_audio_close(a);
    }
    ext[44] = 3;                        /* IEEE float */
    CHECK(codec_audio_open(NULL, ext, sizeof ext, NULL, &a) == -EINVAL, "float samples are refused");

    static const uint8_t png_head[16] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    CHECK(codec_audio_open(NULL, png_head, sizeof png_head, NULL, &a) == -ENOTSUP, "an image is no audio");

    /* Through files. */
    int32_t tone[64];
    for (int i = 0; i < 64; i++)
        tone[i] = (int32_t)((uint32_t)(i * 1000) << 16);
    struct codec_audio_format mono16 = { 16000, 1, 16 };
    mkdir("/tmp", 0755);
    CHECK(codec_audio_save("/tmp/codec.wav", NULL, &mono16, tone, 64) == 0, "save by extension");
    err = codec_audio_open_file("/tmp/codec.wav", &a);
    CHECK(err == 0 && codec_audio_codec(a) == wav && codec_audio_frames(a) == 64, "open the saved file: %d", err);
    if (!err) {
        int32_t back[64];
        CHECK(codec_audio_read(a, back, 64) == 64 && memcmp(back, tone, sizeof tone) == 0, "the saved samples");
        codec_audio_close(a);
    }
    CHECK(codec_audio_save("/tmp/codec.xyz", NULL, &mono16, tone, 64) == -ENOTSUP, "no audio codec for .xyz");
    CHECK(codec_write_file("/tmp/notaudio.wav", png_head, sizeof png_head) == 0, "write a PNG named .wav");
    err = codec_audio_open_file("/tmp/notaudio.wav", &a);
    CHECK(err == -EINVAL, "the extension picks wav, which refuses the data: %d", err);
}

/* ---- FLAC ---- */

/* Decode a whole stream in chunks of 1000 frames. Returns the frames, and
 * the error of the read that failed in *err (0 when the stream ended
 * normally). */
static long decode_all(struct codec_audio *a, int32_t **out, int *err)
{
    int ch = codec_audio_format(a)->channels;
    long cap = 1 << 15, n = 0;
    int32_t *buf = malloc(sizeof *buf * (size_t)cap * ch);
    *err = 0;
    while (buf) {
        if (n + 1000 > cap) {
            int32_t *grown = realloc(buf, sizeof *buf * (size_t)cap * 2 * ch);
            if (!grown) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = grown;
            cap *= 2;
        }
        long got = codec_audio_read(a, buf + n * ch, 1000);
        if (got < 0)
            *err = (int)got;
        if (got <= 0)
            break;
        n += got;
    }
    *out = buf;
    return buf ? n : -1;
}

/* The fixtures with the format that STREAMINFO records. The decoder
 * compares the MD5 sum of each stream with the decoded samples. */
static const struct {
    const char *name;
    int rate, channels, bits;
    long frames;
} flac_fixtures[] = {
    { "codec-stereo16.flac", 44100, 2, 16, 22050 },  { "codec-mono24-lpc32.flac", 48000, 1, 24, 14400 },
    { "codec-u8-3ch.flac", 22050, 3, 8, 6615 },      { "codec-8ch.flac", 8000, 8, 16, 1600 },
    { "codec-wasted.flac", 32000, 2, 16, 4000 },     { "codec-rate12k.flac", 12000, 1, 16, 2000 },
    { "codec-rate11025.flac", 11025, 1, 16, 2000 },  { "codec-rate37800.flac", 37800, 1, 16, 2000 },
    { "codec-rate88200.flac", 88200, 1, 16, 2000 },  { "codec-rate705600.flac", 705600, 1, 16, 2000 },
    { "codec-s12.flac", 16000, 2, 12, 3000 },        { "codec-s20.flac", 16000, 2, 20, 3000 },
    { "codec-s32.flac", 12000, 1, 32, 3000 },        { "codec-ffmpeg24.flac", 96000, 2, 24, 9600 },
    { "codec-variable.flac", 44100, 2, 16, 11000 },  { "codec-id3.flac", 16000, 1, 16, 32000 },
};

/* A test signal of every kind of block: a chord, noise, a constant and a
 * full scale square, quantised to bits. */
static void flac_signal(int32_t *s, long frames, int ch, int bits)
{
    uint32_t seed = 7;
    for (long i = 0; i < frames; i++)
        for (int c = 0; c < ch; c++) {
            long part = i * 4 / frames;
            int32_t v;
            if (part == 0) {
                long phase = i * (97 + 31 * c) % 4096, tri = phase < 2048 ? phase : 4095 - phase;
                v = (int32_t)((tri - 1024) * (1 << 20));
            } else if (part == 1) {
                seed = seed * 1103515245u + 12345u;
                v = (int32_t)seed;
            } else if (part == 2) {
                v = c == 1 ? 0x30000000 : 0;
            } else {
                v = (i / 7) & 1 ? INT32_MAX : INT32_MIN;
            }
            if (bits < 32)
                v = (int32_t)((uint32_t)v & ~((1u << (32 - bits)) - 1));
            s[i * ch + c] = v;
        }
}

static void test_flac(void)
{
    const struct codec *flac = codec_find("flac");
    CHECK(flac && flac->kind == CODEC_AUDIO && flac->caps == (CODEC_DECODE | CODEC_ENCODE), "flac codec");
    CHECK(codec_for_mime(CODEC_AUDIO, "audio/flac", CODEC_DECODE) == flac, "flac by MIME type");
    CHECK(codec_for_path(CODEC_AUDIO, "/home/Song.FLAC", CODEC_ENCODE) == flac, "flac by extension");
    if (!flac)
        return;

    for (size_t i = 0; i < sizeof flac_fixtures / sizeof flac_fixtures[0]; i++) {
        char path[96];
        snprintf(path, sizeof path, "/etc/tests/%s", flac_fixtures[i].name);
        struct codec_audio *a;
        int err = codec_audio_open_file(path, &a);
        CHECK(err == 0 && codec_audio_codec(a) == flac, "open %s: %d", path, err);
        if (err)
            continue;
        const struct codec_audio_format *f = codec_audio_format(a);
        CHECK(f->rate == flac_fixtures[i].rate && f->channels == flac_fixtures[i].channels &&
              f->bits == flac_fixtures[i].bits, "%s format %d %d %d", path, f->rate, f->channels, f->bits);
        int32_t *s;
        long n = decode_all(a, &s, &err);
        CHECK(err == 0 && n == flac_fixtures[i].frames, "%s: %ld frames, error %d", path, n, err);
        printf("codectest: %s %d Hz %d channels %d bit %ld frames\n", flac_fixtures[i].name, f->rate, f->channels,
               f->bits, n);
        free(s);
        codec_audio_close(a);
    }

    /* The chime as FLAC and as WAV. */
    struct codec_audio *a, *b;
    int32_t *sa = NULL, *sb = NULL;
    int ea = codec_audio_open_file("/usr/share/sounds/chime.flac", &a), eb = codec_audio_open_file("/usr/share/sounds/chime.wav", &b);
    long na = ea ? -1 : decode_all(a, &sa, &ea), nb = eb ? -1 : decode_all(b, &sb, &eb);
    CHECK(ea == 0 && eb == 0 && na == nb && na > 0 && memcmp(sa, sb, sizeof *sa * (size_t)na) == 0,
          "the chime as FLAC equals the WAV file: %ld %ld frames", na, nb);
    free(sa);
    free(sb);
    if (!ea)
        codec_audio_close(a);
    if (!eb)
        codec_audio_close(b);

    /* Round trips through the encoder. Some files are retained for the post
     * script, which tests them with the reference flac. */
    static const int sizes[] = { 4, 8, 12, 16, 20, 24, 32 }, layouts[] = { 1, 2, 5, 8 };
    for (int si = 0; si < 7; si++)
        for (int li = 0; li < 4; li++) {
            int bits = sizes[si], ch = layouts[li];
            long frames = 5000 + ch * 37;
            int32_t *in = malloc(sizeof *in * (size_t)frames * ch), *out = NULL;
            if (!in)
                continue;
            flac_signal(in, frames, ch, bits);
            struct codec_audio_format fmt = { 44100, ch, bits };
            uint8_t *file;
            long size = codec_audio_encode(flac, &fmt, in, frames, &file);
            CHECK(size > 0, "encode %d bit %d channels: %ld", bits, ch, size);
            if (size > 0) {
                int err = codec_audio_open(NULL, file, (size_t)size, NULL, &a);
                long n = err ? -1 : decode_all(a, &out, &err);
                CHECK(err == 0 && n == frames && memcmp(in, out, sizeof *in * (size_t)frames * ch) == 0,
                      "round trip at %d bit with %d channels: %ld frames, error %d", bits, ch, n, err);
                if (n >= 0)
                    codec_audio_close(a);
                if ((bits == 16 && ch == 2) || (bits == 24 && ch == 1) || (bits == 32 && ch == 2) ||
                    (bits == 8 && ch == 8) || (bits == 12 && ch == 5)) {
                    char path[48];
                    snprintf(path, sizeof path, "/flac-enc-%d-%d.flac", bits, ch);
                    CHECK(codec_write_file(path, file, (size_t)size) == 0, "write %s", path);
                }
                free(file);
            }
            free(in);
            free(out);
        }
    printf("codectest: flac round trips done\n");

    /* Damage. A changed byte in a frame fails its CRC-16, a changed byte
     * in the MD5 sum of STREAMINFO fails the check at the end, and a cut
     * file ends early. */
    uint8_t *d;
    size_t len;
    if (codec_read_file("/etc/tests/codec-stereo16.flac", &d, &len) == 0) {
        int err;
        int32_t *s;
        d[len / 2] ^= 0x10;
        if (codec_audio_open(NULL, d, len, NULL, &a) == 0) {
            long n = decode_all(a, &s, &err);
            CHECK(err == -EBADMSG && n < 22050, "a damaged frame: %d after %ld frames", err, n);
            free(s);
            codec_audio_close(a);
        }
        d[len / 2] ^= 0x10;
        d[8 + 18] ^= 0x01;              /* the first byte of the MD5 sum */
        if (codec_audio_open(NULL, d, len, NULL, &a) == 0) {
            long n = decode_all(a, &s, &err);
            CHECK(err == -EBADMSG && n == 22050, "a wrong MD5 sum: %d after %ld frames", err, n);
            free(s);
            codec_audio_close(a);
        }
        d[8 + 18] ^= 0x01;
        if (codec_audio_open(NULL, d, len - 300, NULL, &a) == 0) {
            long n = decode_all(a, &s, &err);
            CHECK(err == -EBADMSG && n < 22050, "a cut file: %d after %ld frames", err, n);
            free(s);
            codec_audio_close(a);
        }
        CHECK(codec_audio_open(NULL, d, 40, NULL, &a) == -EINVAL, "a cut header");
        free(d);
    }
}

/* ---- Vorbis ---- */

/* Each fixture decoded by minios and by libvorbis on the host, whose
 * 16 bit samples are stored as FLAC. The decoded samples rounded to 16
 * bits may differ from the reference by one step where the two float
 * computations round differently. */
static void test_vorbis(void)
{
    const struct codec *vorbis = codec_find("vorbis");
    CHECK(vorbis && vorbis->kind == CODEC_AUDIO && (vorbis->caps & CODEC_DECODE), "vorbis codec");
    CHECK(codec_for_mime(CODEC_AUDIO, "audio/ogg", CODEC_DECODE) == vorbis, "vorbis by MIME type");
    CHECK(codec_for_path(CODEC_AUDIO, "/home/Song.OGG", CODEC_DECODE) == vorbis, "vorbis by extension");
    if (!vorbis)
        return;
    static const char *const names[] = { "chime", "stereo", "51", "3ch", "low", "high", "chained", "mux" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char ogg[96], ref[96];
        if (i == 0)
            snprintf(ogg, sizeof ogg, "/usr/share/sounds/chime.ogg");
        else
            snprintf(ogg, sizeof ogg, "/etc/tests/codec-vorbis-%s.ogg", names[i]);
        snprintf(ref, sizeof ref, "/etc/tests/codec-vorbis-%s.ref.flac", names[i]);
        /* The multiplexed file begins with its FLAC stream, which decides
         * the codec found by content, and its Vorbis stream is opened with
         * the Vorbis codec explicitly. */
        struct codec_audio *a, *b;
        uint8_t *mux = NULL;
        size_t muxlen;
        int mux_file = strcmp(names[i], "mux") == 0, ea;
        if (mux_file)
            ea = codec_read_file(ogg, &mux, &muxlen) ? -1 : codec_audio_open(vorbis, mux, muxlen, NULL, &a);
        else
            ea = codec_audio_open_file(ogg, &a);
        int eb = codec_audio_open_file(ref, &b);
        CHECK(ea == 0 && eb == 0, "open %s %d, %s %d", ogg, ea, ref, eb);
        if (ea || eb) {
            if (!ea)
                codec_audio_close(a);
            if (!eb)
                codec_audio_close(b);
            free(mux);
            continue;
        }
        CHECK(codec_audio_codec(a) == vorbis, "%s is identified as Vorbis", ogg);
        const struct codec_audio_format *fa = codec_audio_format(a), *fb = codec_audio_format(b);
        long total = codec_audio_frames(a);
        int32_t *sa, *sb;
        long na = decode_all(a, &sa, &ea), nb = decode_all(b, &sb, &eb);
        long worst = 0;
        double sum = 0;
        long count = 0;
        if (fa->channels == fb->channels && na == nb)
            for (long k = 0; k < na * fa->channels; k++) {
                long d = labs((long)(((int64_t)sa[k] + 0x8000) >> 16) - (long)(sb[k] >> 16));
                if (d > worst)
                    worst = d;
                sum += (double)d * d;
                count++;
            }
        double rms = count ? sqrt(sum / count) : 99;
        CHECK(ea == 0 && eb == 0 && na == nb && total == na && fa->rate == fb->rate && fa->channels == fb->channels,
              "%s: %ld frames (%ld announced), reference %ld, error %d", ogg, na, total, nb, ea);
        CHECK(worst <= 1 && rms < 0.5, "%s differs from libvorbis by up to %ld, rms %.3f", ogg, worst, rms);
        printf("codectest: vorbis %s %d Hz %d channels %ld frames, largest difference %ld, rms %d.%03d\n", names[i],
               fa->rate, fa->channels, na, worst, (int)rms, (int)(rms * 1000) % 1000);
        free(sa);
        free(sb);
        codec_audio_close(a);
        codec_audio_close(b);
        free(mux);
    }

    /* A changed byte in a page fails its CRC, and a cut file ends early. */
    uint8_t *d;
    size_t len;
    if (codec_read_file("/etc/tests/codec-vorbis-stereo.ogg", &d, &len) == 0) {
        struct codec_audio *a;
        int32_t *s;
        int err;
        d[len / 2] ^= 0x40;
        if (codec_audio_open(NULL, d, len, NULL, &a) == 0) {
            long n = decode_all(a, &s, &err);
            CHECK(err == -EBADMSG && n < 66150, "a damaged page: %d after %ld frames", err, n);
            free(s);
            codec_audio_close(a);
        }
        d[len / 2] ^= 0x40;
        if (codec_audio_open(NULL, d, len - 1000, NULL, &a) == 0) {
            long n = decode_all(a, &s, &err);
            CHECK(err == -EBADMSG && n < 66150, "a cut file: %d after %ld frames", err, n);
            free(s);
            codec_audio_close(a);
        }
        free(d);
    }
}

/* ---- the Vorbis encoder ---- */

/* Test signals: a sequence of chords with decaying notes and clicks every
 * half second, which produce short blocks, or a sine per channel with a
 * little noise. */
static void vorbis_signal(int32_t *s, long frames, int ch, int rate, int kind)
{
    static const double notes[6] = { 220, 277.18, 329.63, 440, 554.37, 659.25 };
    uint32_t seed = 11;
    for (long i = 0; i < frames; i++) {
        double t = (double)i / rate;
        for (int c = 0; c < ch; c++) {
            double v = 0;
            seed = seed * 1103515245u + 12345u;
            double noise = (double)(seed >> 8) / (1 << 24) - 0.5;
            if (kind == 0) {
                int seg = (int)(t * 4) % 6;
                double env = exp(-3 * fmod(t * 4, 1));
                for (int k = 0; k < 3; k++)
                    for (int h = 1; h <= 5; h++)
                        v += 0.1 / h * sin(2 * M_PI * notes[(seg + k) % 6] * h * t + c) * env;
                if (i % (rate / 2) < 30)
                    v += 1.2 * noise;
            } else {
                v = 0.3 * sin(2 * M_PI * (200 + 100 * c) * t) + 0.02 * noise;
            }
            v = v > 0.99 ? 0.99 : v < -0.99 ? -0.99 : v;
            s[i * ch + c] = (int32_t)(v * 32767) * 65536;
        }
    }
}

static double snr_db(const int32_t *a, const int32_t *b, long n)
{
    double sig = 0, noise = 0;
    for (long i = 0; i < n; i++) {
        double x = a[i] / 2147483648.0, d = x - b[i] / 2147483648.0;
        sig += x * x;
        noise += d * d;
    }
    return 10 * log10(sig / (noise + 1e-30));
}

/* Encode, decode in minios, and return the signal-to-noise ratio, or -99.
 * With retain set, the file and the decoded 16 bit samples are written for
 * the post script, which decodes the file with libvorbis on the host. */
static double vorbis_round(const char *name, const int32_t *in, long frames, int ch, int rate, const char *options,
                           long *size, const char *retain)
{
    struct codec_audio_format fmt = { rate, ch, 16 };
    uint8_t *file;
    *size = codec_audio_encode_options(codec_find("vorbis"), &fmt, in, frames, options, &file);
    CHECK(*size > 0, "encode %s with %s: %ld", name, options, *size);
    if (*size <= 0)
        return -99;
    struct codec_audio *a;
    int err = codec_audio_open(NULL, file, (size_t)*size, NULL, &a);
    int32_t *out = NULL;
    long n = err ? -1 : decode_all(a, &out, &err);
    CHECK(err == 0 && n == frames && codec_audio_frames(a) == frames, "%s with %s decodes to %ld of %ld frames, error %d",
          name, options, n, frames, err);
    double snr = n == frames ? snr_db(in, out, frames * ch) : -99;
    if (retain && n == frames) {
        char path[64];
        snprintf(path, sizeof path, "/%s.ogg", retain);
        CHECK(codec_write_file(path, file, (size_t)*size) == 0, "write %s", path);
        uint8_t *raw = malloc((size_t)frames * ch * 2);
        for (long i = 0; raw && i < frames * ch; i++) {
            int64_t v = ((int64_t)out[i] + 0x8000) >> 16;
            v = v > 32767 ? 32767 : v;
            raw[2 * i] = (uint8_t)v;
            raw[2 * i + 1] = (uint8_t)(v >> 8);
        }
        snprintf(path, sizeof path, "/%s.raw", retain);
        CHECK(raw && codec_write_file(path, raw, (size_t)frames * ch * 2) == 0, "write %s", path);
        free(raw);
    }
    if (!err || n >= 0)
        codec_audio_close(a);
    free(out);
    free(file);
    printf("codectest: vorbis encoder %s %s: %ld bytes, %d.%d dB\n", name, options, *size, (int)snr,
           (int)(fabs(snr) * 10) % 10);
    return snr;
}

static void test_vorbis_encoder(void)
{
    const struct codec *vorbis = codec_find("vorbis");
    CHECK(vorbis && (vorbis->caps & CODEC_ENCODE) && vorbis->audio_encode_options, "the vorbis encoder");
    if (!vorbis)
        return;
    struct codec_audio *a;
    int32_t *chime = NULL;
    int err = codec_audio_open_file("/usr/share/sounds/chime.wav", &a);
    long nchime = err ? -1 : decode_all(a, &chime, &err);
    if (nchime >= 0)
        codec_audio_close(a);
    long music_n = 44100, six_n = 12000;
    int32_t *music = malloc(sizeof *music * (size_t)music_n * 2), *six = malloc(sizeof *six * (size_t)six_n * 6);
    if (!chime || !music || !six) {
        CHECK(0, "test signals");
        return;
    }
    vorbis_signal(music, music_n, 2, 44100, 0);
    vorbis_signal(six, six_n, 6, 48000, 1);
    /* The signal-to-noise ratio required at each setting, a few dB below
     * the values measured on the host. */
    static const struct {
        int signal;
        const char *options, *retain;
        double snr;
    } runs[] = {
        { 0, "quality=-0.1", NULL, 14 },          { 0, "quality=0.4", NULL, 30 },
        { 0, "quality=1.0", "venc-chime-q10", 49 }, { 0, "floor=0", "venc-chime-floor0", 46 },
        { 1, "quality=0.4", "venc-music-q04", 24 }, { 1, "quality=1.0", NULL, 50 },
        { 1, "floor=0", "venc-music-floor0", 38 },  { 2, "quality=0.4", "venc-six-q04", 24 },
        { 2, "quality=1.0", NULL, 50 },
    };
    long previous = 0;
    for (size_t i = 0; i < sizeof runs / sizeof runs[0]; i++) {
        const int32_t *in = runs[i].signal == 0 ? chime : runs[i].signal == 1 ? music : six;
        long frames = runs[i].signal == 0 ? nchime : runs[i].signal == 1 ? music_n : six_n;
        int ch = runs[i].signal == 0 ? 1 : runs[i].signal == 1 ? 2 : 6;
        int rate = runs[i].signal == 0 ? 16000 : runs[i].signal == 1 ? 44100 : 48000;
        const char *name = runs[i].signal == 0 ? "chime" : runs[i].signal == 1 ? "music" : "5.1";
        long size;
        double snr = vorbis_round(name, in, frames, ch, rate, runs[i].options, &size, runs[i].retain);
        CHECK(snr >= runs[i].snr, "%s with %s: %.1f dB, required %.1f", name, runs[i].options, snr, runs[i].snr);
        /* Within one signal, a higher quality takes more bytes. */
        if (i && runs[i].signal == runs[i - 1].signal && strstr(runs[i].options, "quality") &&
            strstr(runs[i - 1].options, "quality"))
            CHECK(size > previous, "%s: %ld bytes at %s, %ld before", name, size, runs[i].options, previous);
        previous = size;
    }
    uint8_t *file;
    struct codec_audio_format fmt = { 16000, 1, 16 };
    CHECK(codec_audio_encode_options(vorbis, &fmt, chime, nchime, "quality=2", &file) == -EINVAL, "quality 2");
    CHECK(codec_audio_encode_options(vorbis, &fmt, chime, nchime, "speed=1", &file) == -EINVAL, "an unknown option");
    CHECK(codec_audio_encode_options(codec_find("wav"), &fmt, chime, nchime, "quality=1", &file) == -EINVAL,
          "a codec without options");
    struct codec_audio_format fast = { 96000, 1, 16 };
    CHECK(codec_audio_encode_options(vorbis, &fast, chime, nchime, "floor=0", &file) == -EINVAL,
          "floor 0 above 65535 Hz");
    free(chime);
    free(music);
    free(six);
}

/* ---- FLAC in Ogg ---- */

static void test_oggflac(void)
{
    const struct codec *oggflac = codec_find("oggflac");
    CHECK(oggflac && oggflac->caps == (CODEC_DECODE | CODEC_ENCODE), "oggflac codec");
    CHECK(codec_for_mime(CODEC_AUDIO, "audio/x-oggflac", 0) == oggflac, "oggflac by MIME type");
    CHECK(codec_for_path(CODEC_AUDIO, "/x/a.oga", CODEC_ENCODE) == oggflac, "oggflac by extension");
    CHECK(codec_for_path(CODEC_AUDIO, "/x/a.ogg", CODEC_ENCODE) == codec_find("vorbis"), ".ogg remains Vorbis");
    if (!oggflac)
        return;
    static const struct {
        const char *name;
        int rate, channels, bits;
        long frames;
    } files[] = {
        { "codec-oggflac-ref.oga", 44100, 2, 16, 22050 },     { "codec-oggflac-ffmpeg.oga", 48000, 2, 24, 9600 },
        { "codec-oggflac-chained.oga", 16000, 1, 16, 13000 }, { "codec-vorbis-mux.ogg", 22050, 1, 16, 22050 },
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        char path[96];
        snprintf(path, sizeof path, "/etc/tests/%s", files[i].name);
        struct codec_audio *a;
        int err = codec_audio_open_file(path, &a);
        CHECK(err == 0 && codec_audio_codec(a) == oggflac, "%s opens as Ogg FLAC: %d", path, err);
        if (err)
            continue;
        const struct codec_audio_format *f = codec_audio_format(a);
        long announced = codec_audio_frames(a);
        int32_t *s;
        long n = decode_all(a, &s, &err);
        CHECK(err == 0 && n == files[i].frames && announced == n && f->rate == files[i].rate &&
                  f->channels == files[i].channels && f->bits == files[i].bits,
              "%s: %ld frames (%ld announced), %d Hz, %d channels, %d bit, error %d", path, n, announced, f->rate,
              f->channels, f->bits, err);
        printf("codectest: oggflac %s %ld frames\n", files[i].name, n);
        free(s);
        codec_audio_close(a);
    }

    /* Round trips through the encoder, and files for the post script. */
    static const int sizes[] = { 16, 24, 8 }, layouts[] = { 1, 2, 8 };
    for (int k = 0; k < 3; k++) {
        int bits = sizes[k], ch = layouts[k];
        long frames = 9000;
        int32_t *in = malloc(sizeof *in * (size_t)frames * ch), *out = NULL;
        if (!in)
            continue;
        flac_signal(in, frames, ch, bits);
        struct codec_audio_format fmt = { 32000, ch, bits };
        uint8_t *file;
        long size = codec_audio_encode(oggflac, &fmt, in, frames, &file);
        CHECK(size > 0, "oggflac encode %d bit %d channels: %ld", bits, ch, size);
        if (size <= 0) {
            free(in);
            continue;
        }
        struct codec_audio *a;
        int err = codec_audio_open(NULL, file, (size_t)size, NULL, &a);
        long n = err ? -1 : decode_all(a, &out, &err);
        CHECK(err == 0 && n == frames && memcmp(in, out, sizeof *in * (size_t)frames * ch) == 0 &&
                  codec_audio_codec(a) == oggflac,
              "oggflac round trip %d bit %d channels: %ld frames, error %d", bits, ch, n, err);
        if (n >= 0)
            codec_audio_close(a);
        char path[48];
        snprintf(path, sizeof path, "/oggflac-enc-%d-%d.oga", bits, ch);
        CHECK(codec_write_file(path, file, (size_t)size) == 0, "write %s", path);
        /* A changed byte in the middle fails the CRC of its page. */
        file[size / 2] ^= 0x20;
        if (codec_audio_open(NULL, file, (size_t)size, NULL, &a) == 0) {
            int32_t *s;
            long m = decode_all(a, &s, &err);
            CHECK(err == -EBADMSG && m < frames, "a damaged Ogg FLAC page: %d after %ld frames", err, m);
            free(s);
            codec_audio_close(a);
        } else {
            CHECK(0, "a damaged Ogg FLAC file still opens up to the damage");
        }
        free(file);
        free(in);
        free(out);
    }
    printf("codectest: oggflac round trips done\n");
}

/* ---- MP3 ---- */

/* A chord with a tremolo, as the fixtures of the lossy encoders use. */
static void music(int32_t *s, long frames, int channels, int rate)
{
    for (long i = 0; i < frames; i++)
        for (int c = 0; c < channels; c++) {
            double t = (double)i / rate;
            double v = 0.25 * sin(2 * M_PI * (220 + 110 * c) * t) + 0.15 * sin(2 * M_PI * 523.25 * t) +
                       0.1 * sin(2 * M_PI * 1318.5 * t) * (0.5 + 0.5 * sin(2 * M_PI * 2 * t));
            s[i * channels + c] = (int32_t)(v * 2147483647.0);
        }
}

/* The fixtures of LAME against their decoding by FFmpeg, and round trips
 * through the encoder at each MPEG version. */
static void test_mp3(void)
{
    const struct codec *mp3 = codec_find("mp3");
    CHECK(mp3 && mp3->kind == CODEC_AUDIO && (mp3->caps & CODEC_DECODE) && (mp3->caps & CODEC_ENCODE), "mp3 codec");
    CHECK(codec_for_mime(CODEC_AUDIO, "audio/mpeg", CODEC_DECODE) == mp3, "mp3 by MIME type");
    CHECK(codec_for_path(CODEC_AUDIO, "/home/Song.MP3", CODEC_DECODE) == mp3, "mp3 by extension");
    if (!mp3)
        return;
    static const struct { const char *name; long frames; } files[] = {
        { "lame", 66150 }, { "vbr22", 26460 }, { "8k", 13248 } };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        char path[96], ref[96];
        snprintf(path, sizeof path, "/etc/tests/codec-mp3-%s.mp3", files[i].name);
        snprintf(ref, sizeof ref, "/etc/tests/codec-mp3-%s.ref.flac", files[i].name);
        struct codec_audio *a, *b;
        int ea = codec_audio_open_file(path, &a), eb = codec_audio_open_file(ref, &b);
        CHECK(ea == 0 && eb == 0, "open %s %d, %s %d", path, ea, ref, eb);
        if (ea || eb) {
            if (!ea)
                codec_audio_close(a);
            if (!eb)
                codec_audio_close(b);
            continue;
        }
        CHECK(codec_audio_codec(a) == mp3, "%s is identified as MP3", path);
        const struct codec_audio_format *fa = codec_audio_format(a), *fb = codec_audio_format(b);
        long total = codec_audio_frames(a);
        int32_t *sa, *sb;
        long na = decode_all(a, &sa, &ea), nb = decode_all(b, &sb, &eb);
        long worst = 0, count = 0;
        double sum = 0;
        if (fa->channels == fb->channels && na == nb)
            for (long k = 0; k < na * fa->channels; k++) {
                long d = labs((long)(((int64_t)sa[k] + 0x8000) >> 16) - (long)(sb[k] >> 16));
                worst = d > worst ? d : worst;
                sum += (double)d * d;
                count++;
            }
        double rms = count ? sqrt(sum / count) : 99;
        CHECK(ea == 0 && eb == 0 && na == nb && na == files[i].frames && total == na && fa->rate == fb->rate &&
                  fa->channels == fb->channels,
              "%s: %ld frames (%ld announced), reference %ld, error %d", path, na, total, nb, ea);
        CHECK(worst <= 2 && rms < 1, "%s differs from FFmpeg by up to %ld, rms %.3f", path, worst, rms);
        printf("codectest: mp3 %s %d Hz %d channels %ld frames, largest difference %ld\n", files[i].name, fa->rate,
               fa->channels, na, worst);
        free(sa);
        free(sb);
        codec_audio_close(a);
        codec_audio_close(b);
    }

    static const struct { int rate, channels; const char *options; double min_snr; } runs[] = {
        { 44100, 2, NULL, 15 }, { 48000, 1, "bitrate=64", 15 }, { 22050, 2, NULL, 15 },
        { 16000, 1, "bitrate=32", 15 }, { 8000, 1, "bitrate=16", 10 } };
    for (size_t r = 0; r < sizeof runs / sizeof runs[0]; r++) {
        struct codec_audio_format f = { runs[r].rate, runs[r].channels, 16 };
        long n = runs[r].rate + 777;
        int32_t *in = malloc(sizeof *in * (size_t)n * f.channels), *out = NULL;
        music(in, n, f.channels, f.rate);
        uint8_t *file;
        long size = codec_audio_encode_options(mp3, &f, in, n, runs[r].options, &file);
        struct codec_audio *a;
        int err = size > 0 ? codec_audio_open(NULL, file, (size_t)size, NULL, &a) : (int)size;
        long m = err ? -1 : decode_all(a, &out, &err);
        double snr = m == n ? snr_db(in, out, n * f.channels) : -99;
        CHECK(size > 0 && err == 0 && m == n && snr > runs[r].min_snr, "mp3 round trip %d Hz %d channels: %ld of %ld frames, %d dB",
              f.rate, f.channels, m, n, (int)snr);
        printf("codectest: mp3 encoder %d Hz %d channels: %ld bytes, %d dB\n", f.rate, f.channels, size, (int)snr);
        if (m >= 0)
            codec_audio_close(a);
        free(in);
        free(out);
        if (size > 0)
            free(file);
    }
    struct codec_audio_format bad = { 96000, 2, 16 }, ok = { 44100, 2, 16 };
    int32_t silence[64] = { 0 };
    uint8_t *file;
    CHECK(codec_audio_encode(mp3, &bad, silence, 10, &file) == -EINVAL, "mp3 refuses 96 kHz");
    CHECK(codec_audio_encode_options(mp3, &ok, silence, 10, "bitrate=100", &file) == -EINVAL, "mp3 refuses 100 kbit/s");
    /* A cut file ends early without an error. */
    uint8_t *d;
    size_t len;
    if (codec_read_file("/etc/tests/codec-mp3-lame.mp3", &d, &len) == 0) {
        struct codec_audio *a;
        int32_t *s;
        int err;
        if (codec_audio_open(NULL, d, len / 2, NULL, &a) == 0) {
            long n = decode_all(a, &s, &err);
            CHECK(err == 0 && n > 20000 && n < 66150, "a cut MP3 file: %d after %ld frames", err, n);
            free(s);
            codec_audio_close(a);
        }
        free(d);
    }
}

/* ---- GIF ---- */

/* The pixels of one frame of a reference strip, which ImageMagick stacked
 * from top to bottom. A transparent pixel matches any transparent pixel. */
static int same_frame(const uint32_t *pixels, const uint32_t *strip, int index, int w, int h)
{
    for (int i = 0; i < w * h; i++) {
        uint32_t a = pixels[i], b = strip[(size_t)index * w * h + i];
        if (!((a >> 24) == 0 && (b >> 24) == 0) && a != b)
            return 0;
    }
    return 1;
}

static void test_gif(void)
{
    const struct codec *gif = codec_find("gif");
    CHECK(gif && gif->kind == CODEC_IMAGE && (gif->caps & CODEC_ANIMATED) && (gif->caps & CODEC_ENCODE), "gif codec");
    CHECK(codec_for_mime(CODEC_IMAGE, "image/gif", CODEC_DECODE) == gif, "gif by MIME type");
    CHECK(codec_for_path(CODEC_IMAGE, "/home/Cat.GIF", CODEC_DECODE | CODEC_ANIMATED) == gif, "gif by extension");
    if (!gif)
        return;
    static const struct { const char *name; int frames, loops, delays[4]; } files[] = {
        { "pillow", 4, 3, { 100, 200, 300, 500 } }, { "previous", 4, 0, { 70, 70, 70, 70 } },
        { "interlaced", 1, 1, { 100 } } };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        char path[96], ref[96];
        snprintf(path, sizeof path, "/etc/tests/codec-gif-%s.gif", files[i].name);
        snprintf(ref, sizeof ref, "/etc/tests/codec-gif-%s.ref.png", files[i].name);
        struct codec_picture strip;
        struct codec_animation *a;
        int er = codec_image_load(ref, NULL, &strip), ea = codec_animation_open_file(path, &a);
        CHECK(er == 0 && ea == 0, "open %s %d %d", path, er, ea);
        if (er || ea) {
            if (!er)
                codec_picture_free(&strip);
            if (!ea)
                codec_animation_close(a);
            continue;
        }
        const struct codec_animation_info *info = codec_animation_info(a);
        int w = info->w, h = info->h, n = 0, delay, r;
        CHECK(info->frames == files[i].frames && info->loops == files[i].loops && strip.w == w &&
                  strip.h == h * files[i].frames,
              "%s: %dx%d, %d frames, loops %d", path, w, h, info->frames, info->loops);
        uint32_t *pixels = malloc(sizeof *pixels * (size_t)w * h);
        while ((r = codec_animation_next(a, pixels, &delay)) == 1 && n < files[i].frames) {
            CHECK(same_frame(pixels, strip.pixels, n, w, h), "%s: frame %d differs from ImageMagick", path, n);
            CHECK(delay == files[i].delays[n], "%s: frame %d delay %d", path, n, delay);
            n++;
        }
        CHECK(r == 0 && n == files[i].frames, "%s: %d frames, end %d", path, n, r);
        CHECK(codec_animation_rewind(a) == 0 && codec_animation_next(a, pixels, &delay) == 1 &&
                  same_frame(pixels, strip.pixels, 0, w, h),
              "%s: rewind", path);
        struct image *first = image_load(path);
        CHECK(first && first->w == w && first->h == h && same_frame(first->pixels, strip.pixels, 0, w, h),
              "%s: the first frame through libgui", path);
        image_free(first);
        printf("codectest: gif %s %dx%d, %d frames, loops %d\n", files[i].name, w, h, n, info->loops);
        free(pixels);
        codec_picture_free(&strip);
        codec_animation_close(a);
    }

    /* At most 255 colours and transparency: the round trip is exact. */
    enum { W = 37, H = 23 };
    static uint32_t frames[3][W * H];
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < W * H; i++) {
            int x = i % W, y = i / W;
            frames[k][i] = (x + y + k) % 7 == 0 ? 0 : 0xff000000u | (uint32_t)((x / 5 * 30 + k * 20) % 250) << 16 |
                                                          (uint32_t)(y / 4 * 40) << 8 | (uint32_t)(k * 70);
        }
    struct codec_picture pic = { W, H, frames[0] }, back;
    uint8_t *file;
    long size = codec_image_encode(gif, &pic, &file);
    CHECK(size > 0 && codec_image_decode(NULL, file, (size_t)size, NULL, NULL, &back) == 0 && back.w == W &&
              back.h == H && same_frame(back.pixels, frames[0], 0, W, H),
          "gif still round trip: %ld bytes", size);
    if (size > 0) {
        codec_picture_free(&back);
        free(file);
    }
    struct codec_animation_info info = { W, H, 3, 0 };
    struct codec_frame f[3] = { { frames[0], 40 }, { frames[1], 120 }, { frames[2], 1000 } };
    CHECK(codec_animation_save("/tmp/anim.gif", NULL, &info, f, 3) == 0, "save an animation by extension");
    struct codec_animation *a;
    if (codec_animation_open_file("/tmp/anim.gif", &a) == 0) {
        const struct codec_animation_info *got = codec_animation_info(a);
        CHECK(got->frames == 3 && got->loops == 0, "the saved animation: %d frames, loops %d", got->frames, got->loops);
        uint32_t pixels[W * H];
        int delay;
        for (int k = 0; k < 3; k++)
            CHECK(codec_animation_next(a, pixels, &delay) == 1 && same_frame(pixels, frames[k], 0, W, H) &&
                      delay == f[k].delay_ms,
                  "the saved animation, frame %d: delay %d", k, delay);
        codec_animation_close(a);
    } else {
        CHECK(0, "open the saved animation");
    }
    struct codec_animation *none;
    CHECK(codec_animation_open_file("/usr/share/icons/folder.png", &none) == -ENOTSUP, "a PNG file has no animation");
    CHECK(codec_animation_encode(codec_find("png"), &info, f, 3, &file) == -ENOTSUP, "the PNG codec has no animations");
    /* A cut file: the frames before the cut. */
    uint8_t *d;
    size_t len;
    if (codec_read_file("/etc/tests/codec-gif-pillow.gif", &d, &len) == 0) {
        struct codec_animation *c;
        if (codec_animation_open(NULL, d, len / 2, NULL, &c) == 0) {
            uint32_t pixels[48 * 32];
            int delay, k = 0;
            while (codec_animation_next(c, pixels, &delay) == 1)
                k++;
            CHECK(k >= 1 && k < 4, "a cut GIF file: %d frames", k);
            codec_animation_close(c);
        }
        free(d);
    }
}

static void list_registry(void)
{
    int modules = codec_module_count();
    for (int i = 0; i < modules; i++)
        printf("codectest: module %s from %s\n", codec_module_get(i)->name, codec_module_path(i));
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "count") == 0) {
        printf("%d\n", codec_count());
        return 0;
    }
    static const char *const sections[] = { "image", "audio", "flac", "vorbis", "vorbisenc", "oggflac", "mp3", "gif" };
    int known = 0;
    for (size_t i = 0; i < sizeof sections / sizeof sections[0]; i++)
        known |= argc > 1 && strcmp(argv[1], sections[i]) == 0;
#define WANTS(name) (!known || strcmp(argv[1], name) == 0)
    list_registry();
    if (WANTS("image")) {
        test_registry();
        test_probe();
        test_images();
        test_path();
    }
    if (WANTS("audio"))
        test_audio();
    if (WANTS("flac"))
        test_flac();
    if (WANTS("vorbis"))
        test_vorbis();
    if (WANTS("vorbisenc"))
        test_vorbis_encoder();
    if (WANTS("oggflac"))
        test_oggflac();
    if (WANTS("mp3"))
        test_mp3();
    if (WANTS("gif"))
        test_gif();
    printf("codectest: %d failures\n", failures);
    return failures ? 1 : 0;
}
