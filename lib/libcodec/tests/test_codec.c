/* Host tests of libcodec, run by make check with every module compiled
 * in: MD5 against the vectors of RFC 1321, the inverse MDCT of Vorbis
 * against its definition, the FLAC fixtures with their MD5 sums, the
 * Vorbis and MP3 fixtures against their decoding by libvorbis and FFmpeg,
 * the GIF fixtures against the frames that ImageMagick composes, and round
 * trips through the encoders of Vorbis, MP3 and GIF. The boot tests
 * codec_flac, codec_vorbis, codec_mp3 and codec_gif repeat the fixture
 * checks in minios. */
#include <codec/codec.h>
#include "../modules/vorbis/vorbis.h"
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static const char *fixtures = "../../user/etc/tests", *sounds = "../../user/share/sounds";

static void test_md5(void)
{
    static const char *const in[] = { "", "abc", "message digest",
                                      "12345678901234567890123456789012345678901234567890123456789012345678901234567890" };
    static const char *const out[] = { "d41d8cd98f00b204e9800998ecf8427e", "900150983cd24fb0d6963f7d28e17f72",
                                       "f96b697d7cb7938d525a2f31aaf161d0", "57edf4a22be3c955ac49da2e2107b67a" };
    for (int i = 0; i < 4; i++) {
        struct codec_md5 m;
        uint8_t d[16];
        char hex[33];
        codec_md5_init(&m);
        codec_md5_update(&m, in[i], strlen(in[i]));
        codec_md5_final(&m, d);
        for (int j = 0; j < 16; j++)
            snprintf(hex + 2 * j, 3, "%02x", d[j]);
        CHECK(strcmp(hex, out[i]) == 0, "MD5 of \"%s\": %s", in[i], hex);
    }
}

static void test_imdct(void)
{
    for (unsigned n = 64; n <= 2048; n *= 2) {
        struct codec_mdct m;
        CHECK(codec_mdct_init(&m, n) == 0, "mdct init %u", n);
        float *in = malloc(sizeof *in * n / 2), *a = malloc(sizeof *a * n), *b = malloc(sizeof *b * n);
        for (unsigned i = 0; i < n / 2; i++)
            in[i] = (float)(sin(i * 1.7) * 0.5 + cos(i * 0.31));
        codec_imdct(&m, in, a);
        codec_imdct_direct(n, in, b);
        double err = 0, mag = 0;
        for (unsigned i = 0; i < n; i++) {
            err = fmax(err, fabs((double)a[i] - b[i]));
            mag = fmax(mag, fabs((double)b[i]));
        }
        CHECK(err < 1e-6 * mag * n / 64 + 1e-5, "inverse MDCT of %u: error %g of %g", n, err, mag);
        codec_mdct_free(&m);
        free(in);
        free(a);
        free(b);
    }
}

/* Decode a file, with codec c or the codec found by content when c is
 * NULL. */
static long decode_with(const struct codec *c, const char *path, int32_t **out, struct codec_audio_format *fmt,
                        int *err)
{
    struct codec_audio *a;
    uint8_t *data = NULL;
    size_t len;
    *out = NULL;
    if (c) {
        *err = codec_read_file(path, &data, &len);
        if (!*err)
            *err = codec_audio_open(c, data, len, path, &a);
    } else {
        *err = codec_audio_open_file(path, &a);
    }
    if (*err) {
        free(data);
        return -1;
    }
    *fmt = *codec_audio_format(a);
    long cap = 1 << 16, n = 0;
    int32_t *buf = malloc(sizeof *buf * cap * fmt->channels);
    for (;;) {
        if (n + 4096 > cap) {
            cap *= 2;
            buf = realloc(buf, sizeof *buf * cap * fmt->channels);
        }
        long got = codec_audio_read(a, buf + n * fmt->channels, 4096);
        if (got < 0)
            *err = (int)got;
        if (got <= 0)
            break;
        n += got;
    }
    codec_audio_close(a);
    free(data);
    *out = buf;
    return n;
}

static long decode(const char *path, int32_t **out, struct codec_audio_format *fmt, int *err)
{
    return decode_with(NULL, path, out, fmt, err);
}

static void test_flac(void)
{
    DIR *d = opendir(fixtures);
    struct dirent *e;
    int count = 0;
    while (d && (e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (strncmp(e->d_name, "codec-", 6) != 0 || len < 5 || strcmp(e->d_name + len - 5, ".flac") != 0)
            continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", fixtures, e->d_name);
        int32_t *s;
        struct codec_audio_format f;
        int err;
        long n = decode(path, &s, &f, &err);
        CHECK(err == 0 && n > 0, "%s: %ld frames, error %d", e->d_name, n, err);
        free(s);
        count++;
    }
    if (d)
        closedir(d);
    CHECK(count >= 16, "%d FLAC fixtures", count);
    /* FLAC in Ogg: the fixtures, and the FLAC stream of the multiplexed
     * file, which identification by content selects. */
    static const char *const ogg[] = { "codec-oggflac-ref.oga", "codec-oggflac-ffmpeg.oga",
                                       "codec-oggflac-chained.oga", "codec-vorbis-mux.ogg" };
    static const long lengths[] = { 22050, 9600, 13000, 22050 };
    for (int i = 0; i < 4; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", fixtures, ogg[i]);
        int32_t *s;
        struct codec_audio_format f;
        int err;
        long n = decode(path, &s, &f, &err);
        CHECK(err == 0 && n == lengths[i], "%s: %ld frames, error %d", ogg[i], n, err);
        free(s);
    }
}

static void test_vorbis(void)
{
    static const char *const names[] = { "chime", "stereo", "51", "3ch", "low", "high", "chained", "mux" };
    for (int i = 0; i < 8; i++) {
        char ogg[512], ref[512];
        if (i == 0)
            snprintf(ogg, sizeof ogg, "%s/chime.ogg", sounds);
        else
            snprintf(ogg, sizeof ogg, "%s/codec-vorbis-%s.ogg", fixtures, names[i]);
        snprintf(ref, sizeof ref, "%s/codec-vorbis-%s.ref.flac", fixtures, names[i]);
        int32_t *a, *b;
        struct codec_audio_format fa, fb;
        int ea, eb;
        /* The multiplexed file starts with its FLAC stream, and its Vorbis
         * stream is opened with the Vorbis codec explicitly. */
        long na = decode_with(codec_find("vorbis"), ogg, &a, &fa, &ea), nb = decode(ref, &b, &fb, &eb);
        long worst = 0;
        double sum = 0;
        for (long k = 0; na == nb && fa.channels == fb.channels && k < na * fa.channels; k++) {
            long diff = labs((long)(((int64_t)a[k] + 0x8000) >> 16) - (long)(b[k] >> 16));
            worst = diff > worst ? diff : worst;
            sum += (double)diff * diff;
        }
        double rms = na > 0 ? sqrt(sum / ((double)na * fa.channels)) : 99;
        CHECK(ea == 0 && eb == 0 && na == nb && fa.channels == fb.channels && worst <= 1 && rms < 0.5,
              "vorbis %s: %ld/%ld frames, %d/%d channels, error %d, %ld, rms %.3f", names[i], na, nb, fa.channels,
              fb.channels, ea, worst, rms);
        free(a);
        free(b);
    }
}

/* The chime through the Vorbis encoder and decoder. */
static void test_vorbis_encoder(void)
{
    int32_t *in, *out;
    struct codec_audio_format f;
    int err;
    char path[512];
    snprintf(path, sizeof path, "%s/chime.wav", sounds);
    long n = decode(path, &in, &f, &err);
    uint8_t *file;
    long size = codec_audio_encode_options(codec_find("vorbis"), &f, in, n, "quality=0.4", &file);
    CHECK(size > 0, "vorbis encode: %ld", size);
    if (size <= 0)
        return;
    codec_write_file("/tmp/libcodec-test.ogg", file, (size_t)size);
    struct codec_audio_format g;
    long m = decode("/tmp/libcodec-test.ogg", &out, &g, &err);
    double sig = 0, noise = 0;
    for (long i = 0; i < n && m == n; i++) {
        double a = in[i] / 2147483648.0, b = out[i] / 2147483648.0;
        sig += a * a;
        noise += (a - b) * (a - b);
    }
    double snr = 10 * log10(sig / (noise + 1e-30));
    CHECK(err == 0 && m == n && snr > 30, "vorbis round trip: %ld of %ld frames, %.1f dB", m, n, snr);
    free(in);
    free(out);
    free(file);
}

/* The MP3 fixtures of LAME against their decoding by FFmpeg. minimp3 and
 * FFmpeg both decode in floating point, and their 16 bit results differ
 * by one step at most. */
static void test_mp3(void)
{
    static const struct { const char *name; long frames; } files[] = {
        { "lame", 66150 }, { "vbr22", 26460 }, { "8k", 13248 } };
    for (int i = 0; i < 3; i++) {
        char mp3[512], ref[512];
        snprintf(mp3, sizeof mp3, "%s/codec-mp3-%s.mp3", fixtures, files[i].name);
        snprintf(ref, sizeof ref, "%s/codec-mp3-%s.ref.flac", fixtures, files[i].name);
        int32_t *a, *b;
        struct codec_audio_format fa, fb;
        int ea, eb;
        long na = decode(mp3, &a, &fa, &ea), nb = decode(ref, &b, &fb, &eb);
        long worst = 0;
        double sum = 0;
        for (long k = 0; na == nb && fa.channels == fb.channels && k < na * fa.channels; k++) {
            long diff = labs((long)(((int64_t)a[k] + 0x8000) >> 16) - (long)(b[k] >> 16));
            worst = diff > worst ? diff : worst;
            sum += (double)diff * diff;
        }
        double rms = na > 0 ? sqrt(sum / ((double)na * fa.channels)) : 99;
        CHECK(ea == 0 && eb == 0 && na == nb && na == files[i].frames && fa.rate == fb.rate &&
                  fa.channels == fb.channels && worst <= 2 && rms < 1,
              "mp3 %s: %ld/%ld frames, %d/%d channels, error %d, largest difference %ld, rms %.3f", files[i].name, na,
              nb, fa.channels, fb.channels, ea, worst, rms);
        free(a);
        free(b);
    }
}

/* A chord with a tremolo, as the fixtures of the encoders use. */
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

static double snr(const int32_t *a, const int32_t *b, long n)
{
    double sig = 0, noise = 0;
    for (long i = 0; i < n; i++) {
        double x = a[i] / 2147483648.0, d = x - b[i] / 2147483648.0;
        sig += x * x;
        noise += d * d;
    }
    return 10 * log10(sig / (noise + 1e-30));
}

/* The MP3 encoder at every MPEG version. The decoded stream must have the
 * length of the input and line up with it sample by sample. */
static void test_mp3_encoder(void)
{
    static const struct { int rate, channels; const char *options; double min_snr; } runs[] = {
        { 44100, 2, NULL, 15 }, { 48000, 1, "bitrate=64", 15 }, { 32000, 2, "bitrate=96", 15 },
        { 22050, 2, NULL, 15 }, { 16000, 1, "bitrate=32", 15 }, { 8000, 1, "bitrate=16", 10 },
        { 11025, 2, NULL, 10 } };
    const struct codec *mp3 = codec_find("mp3");
    for (size_t r = 0; r < sizeof runs / sizeof runs[0]; r++) {
        struct codec_audio_format f = { runs[r].rate, runs[r].channels, 16 };
        long n = runs[r].rate + 777;
        int32_t *in = malloc(sizeof *in * n * f.channels), *out;
        music(in, n, f.channels, f.rate);
        uint8_t *file;
        long size = codec_audio_encode_options(mp3, &f, in, n, runs[r].options, &file);
        CHECK(size > 0, "mp3 encode %d Hz: %ld", f.rate, size);
        if (size <= 0) {
            free(in);
            continue;
        }
        codec_write_file("/tmp/libcodec-test.mp3", file, (size_t)size);
        struct codec_audio_format g;
        int err;
        long m = decode("/tmp/libcodec-test.mp3", &out, &g, &err);
        double db = m == n ? snr(in, out, n * f.channels) : -99;
        CHECK(err == 0 && m == n && g.rate == f.rate && g.channels == f.channels && db > runs[r].min_snr,
              "mp3 round trip %d Hz %d channels: %ld of %ld frames, %.1f dB", f.rate, f.channels, m, n, db);
        CHECK(memcmp(file + (f.rate >= 32000 ? (f.channels == 1 ? 21 : 36) : (f.channels == 1 ? 13 : 21)), "Info", 4) == 0,
              "mp3 %d Hz: the Info tag", f.rate);
        free(in);
        free(out);
        free(file);
    }
    struct codec_audio_format bad[] = { { 96000, 2, 16 }, { 44100, 3, 16 } };
    int32_t silence[64] = { 0 };
    uint8_t *file;
    for (int i = 0; i < 2; i++)
        CHECK(codec_audio_encode(mp3, &bad[i], silence, 10, &file) == -EINVAL, "mp3 refuses %d Hz %d channels",
              bad[i].rate, bad[i].channels);
    struct codec_audio_format ok = { 44100, 2, 16 };
    CHECK(codec_audio_encode_options(mp3, &ok, silence, 10, "bitrate=100", &file) == -EINVAL, "mp3 refuses 100 kbit/s");
    CHECK(codec_audio_encode_options(mp3, &ok, silence, 10, "quality=1", &file) == -EINVAL, "mp3 refuses quality");
}

/* Every rate, bit rate and channel count that MPEG allows. Each file
 * must decode to the length of the input, and its frames must follow each
 * other at the lengths that their headers give. */
static void test_mp3_configurations(void)
{
    static const int rates[9] = { 44100, 48000, 32000, 22050, 24000, 16000, 11025, 12000, 8000 };
    static const int kbps[] = { 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 192, 224, 256, 320 };
    static const int mpeg1[15] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
    static const int mpeg2[15] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 };
    const struct codec *mp3 = codec_find("mp3");
    int encodable = 0, good = 0;
    for (int r = 0; r < 9; r++)
        for (size_t b = 0; b < sizeof kbps / sizeof kbps[0]; b++)
            for (int ch = 1; ch <= 2; ch++) {
                struct codec_audio_format f = { rates[r], ch, 16 };
                long n = rates[r] / 5;
                int32_t *in = malloc(sizeof *in * n * ch);
                music(in, n, ch, f.rate);
                char options[32];
                snprintf(options, sizeof options, "bitrate=%d", kbps[b]);
                uint8_t *file;
                long size = codec_audio_encode_options(mp3, &f, in, n, options, &file);
                free(in);
                if (size == -EINVAL)
                    continue;
                encodable++;
                if (size <= 0)
                    continue;
                size_t at = 0;
                while (at + 4 <= (size_t)size && file[at] == 0xff) {
                    int version = file[at + 1] >> 3 & 3, rate = rates[(file[at + 2] >> 2 & 3) + (version == 3 ? 0 : version == 2 ? 3 : 6)];
                    int k = (version == 3 ? mpeg1 : mpeg2)[file[at + 2] >> 4];
                    at += (size_t)((version == 3 ? 144000 : 72000) * k / rate + (file[at + 2] >> 1 & 1));
                }
                struct codec_audio *a;
                int32_t *out = malloc(sizeof *out * (n + 4096) * ch);
                long m = -1;
                if (at == (size_t)size && codec_audio_open(NULL, file, (size_t)size, NULL, &a) == 0) {
                    m = 0;
                    long got;
                    while ((got = codec_audio_read(a, out + m * ch, 4096)) > 0)
                        m += got;
                    codec_audio_close(a);
                }
                good += m == n;
                CHECK(m == n, "mp3 %d Hz %d kbit/s %d channels: frames end at %zu of %ld bytes, %ld of %ld samples",
                      rates[r], kbps[b], ch, at, size, m, n);
                free(out);
                free(file);
            }
    CHECK(encodable == 216 && good == 216, "mp3: %d of %d configurations", good, encodable);
}

/* The pixels of one frame of a reference strip, which ImageMagick stacked
 * from top to bottom. A transparent pixel matches any transparent pixel. */
static int same_frame(const uint32_t *pixels, const struct codec_picture *strip, int index, int w, int h)
{
    for (int i = 0; i < w * h; i++) {
        uint32_t a = pixels[i], b = strip->pixels[(size_t)index * w * h + i];
        if ((a >> 24) == 0 && (b >> 24) == 0)
            continue;
        if (a != b)
            return 0;
    }
    return 1;
}

static void test_gif(void)
{
    static const struct { const char *name; int frames, loops, delays[4]; } files[] = {
        { "pillow", 4, 3, { 100, 200, 300, 500 } }, { "previous", 4, 0, { 70, 70, 70, 70 } },
        { "interlaced", 1, 1, { 100 } } };
    for (int i = 0; i < 3; i++) {
        char gif[512], ref[512];
        snprintf(gif, sizeof gif, "%s/codec-gif-%s.gif", fixtures, files[i].name);
        snprintf(ref, sizeof ref, "%s/codec-gif-%s.ref.png", fixtures, files[i].name);
        struct codec_picture strip, first;
        struct codec_animation *a;
        int er = codec_image_load(ref, NULL, &strip), ea = codec_animation_open_file(gif, &a);
        CHECK(er == 0 && ea == 0, "open %s %d %d", files[i].name, er, ea);
        if (er || ea)
            continue;
        const struct codec_animation_info *info = codec_animation_info(a);
        int w = info->w, h = info->h;
        CHECK(info->frames == files[i].frames && info->loops == files[i].loops && strip.w == w &&
                  strip.h == h * files[i].frames,
              "gif %s: %dx%d, %d frames, loops %d", files[i].name, w, h, info->frames, info->loops);
        uint32_t *pixels = malloc(sizeof *pixels * w * h);
        int n = 0, delay, r;
        while ((r = codec_animation_next(a, pixels, &delay)) == 1 && n < files[i].frames) {
            CHECK(same_frame(pixels, &strip, n, w, h), "gif %s: frame %d differs from ImageMagick", files[i].name, n);
            CHECK(delay == files[i].delays[n], "gif %s: frame %d delay %d", files[i].name, n, delay);
            n++;
        }
        CHECK(r == 0 && n == files[i].frames, "gif %s: %d frames decoded, end %d", files[i].name, n, r);
        CHECK(codec_animation_rewind(a) == 0 && codec_animation_next(a, pixels, &delay) == 1 &&
                  same_frame(pixels, &strip, 0, w, h),
              "gif %s: rewind", files[i].name);
        CHECK(codec_image_load(gif, NULL, &first) == 0 && first.w == w && first.h == h &&
                  same_frame(first.pixels, &strip, 0, w, h),
              "gif %s: the first frame as an image", files[i].name);
        codec_picture_free(&first);
        codec_picture_free(&strip);
        free(pixels);
        codec_animation_close(a);
    }
}

static void test_gif_encoder(void)
{
    const struct codec *gif = codec_find("gif");
    /* At most 255 colours and transparency: the round trip is exact. */
    enum { W = 37, H = 23 };
    uint32_t frames[3][W * H];
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
              back.h == H && same_frame(back.pixels, &pic, 0, W, H),
          "gif still round trip: %ld bytes", size);
    codec_picture_free(&back);
    free(file);
    static const int loops[3] = { 0, 1, 5 };
    for (int l = 0; l < 3; l++) {
        struct codec_animation_info info = { W, H, 3, loops[l] };
        struct codec_frame f[3] = { { frames[0], 40 }, { frames[1], 120 }, { frames[2], 1000 } };
        size = codec_animation_encode(gif, &info, f, 3, &file);
        struct codec_animation *a;
        CHECK(size > 0 && codec_animation_open(NULL, file, (size_t)size, NULL, &a) == 0, "gif animation %d", l);
        if (size <= 0)
            continue;
        const struct codec_animation_info *got = codec_animation_info(a);
        CHECK(got->frames == 3 && got->loops == loops[l] && got->w == W && got->h == H, "gif animation loops %d: %d",
              loops[l], got->loops);
        uint32_t pixels[W * H];
        int delay;
        for (int k = 0; k < 3; k++) {
            struct codec_picture ref = { W, H, frames[k] };
            CHECK(codec_animation_next(a, pixels, &delay) == 1 && same_frame(pixels, &ref, 0, W, H) &&
                      delay == f[k].delay_ms,
                  "gif animation frame %d: delay %d", k, delay);
        }
        CHECK(codec_animation_next(a, pixels, &delay) == 0, "gif animation end");
        codec_animation_close(a);
        free(file);
    }
    /* Many colours: median cut. */
    enum { PW = 160, PH = 120 };
    uint32_t *photo = malloc(sizeof *photo * PW * PH);
    for (int i = 0; i < PW * PH; i++) {
        int x = i % PW, y = i / PW;
        photo[i] = 0xff000000u | (uint32_t)(x * 255 / PW) << 16 | (uint32_t)(y * 255 / PH) << 8 |
                   (uint32_t)(128 + 127 * sin(x * 0.07 + y * 0.05));
    }
    pic = (struct codec_picture){ PW, PH, photo };
    size = codec_image_encode(gif, &pic, &file);
    double err = 0;
    int ok = size > 0 && codec_image_decode(NULL, file, (size_t)size, NULL, NULL, &back) == 0;
    for (int i = 0; ok && i < PW * PH; i++)
        for (int sh = 0; sh < 24; sh += 8) {
            double d = (double)(photo[i] >> sh & 0xff) - (back.pixels[i] >> sh & 0xff);
            err += d * d;
        }
    double psnr = ok ? 10 * log10(255.0 * 255.0 / (err / (PW * PH * 3) + 1e-9)) : 0;
    CHECK(ok && psnr > 28, "gif median cut: %ld bytes, PSNR %.1f dB", size, psnr);
    if (ok)
        codec_picture_free(&back);
    free(file);
    free(photo);
    struct codec_animation_info info = { W, H, 0, 0 };
    CHECK(codec_animation_encode(gif, &info, NULL, 0, &file) == -EINVAL, "gif refuses no frames");
    CHECK(codec_animation_encode(codec_find("png"), &info, NULL, 1, &file) == -ENOTSUP, "png has no animations");
}

int main(void)
{
    test_md5();
    test_imdct();
    test_flac();
    test_vorbis();
    test_vorbis_encoder();
    test_mp3();
    test_mp3_encoder();
    test_mp3_configurations();
    test_gif();
    test_gif_encoder();
    printf("libcodec tests: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
