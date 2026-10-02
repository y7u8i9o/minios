/* Host tests of libcodec, run by make check with every module compiled
 * in: MD5 against the vectors of RFC 1321, the inverse MDCT of Vorbis
 * against its definition, the FLAC fixtures with their MD5 sums, and the
 * Vorbis fixtures against their decoding by libvorbis. The boot tests
 * codec_flac and codec_vorbis repeat the fixture checks in minios. */
#include <codec/codec.h>
#include "../modules/vorbis/vorbis.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static const char *fixtures = "../user/etc/tests", *sounds = "../user/share/sounds";

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
        struct vb_mdct m;
        CHECK(vb_mdct_init(&m, n) == 0, "mdct init %u", n);
        float *in = malloc(sizeof *in * n / 2), *a = malloc(sizeof *a * n), *b = malloc(sizeof *b * n);
        for (unsigned i = 0; i < n / 2; i++)
            in[i] = (float)(sin(i * 1.7) * 0.5 + cos(i * 0.31));
        vb_imdct(&m, in, a);
        vb_imdct_direct(n, in, b);
        double err = 0, mag = 0;
        for (unsigned i = 0; i < n; i++) {
            err = fmax(err, fabs((double)a[i] - b[i]));
            mag = fmax(mag, fabs((double)b[i]));
        }
        CHECK(err < 1e-6 * mag * n / 64 + 1e-5, "inverse MDCT of %u: error %g of %g", n, err, mag);
        vb_mdct_free(&m);
        free(in);
        free(a);
        free(b);
    }
}

static long decode(const char *path, int32_t **out, struct codec_audio_format *fmt, int *err)
{
    struct codec_audio *a;
    *err = codec_audio_open_file(path, &a);
    if (*err)
        return -1;
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
    *out = buf;
    return n;
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
        long na = decode(ogg, &a, &fa, &ea), nb = decode(ref, &b, &fb, &eb);
        long worst = 0;
        double sum = 0;
        for (long k = 0; na == nb && fa.channels == fb.channels && k < na * fa.channels; k++) {
            long diff = labs((long)(((int64_t)a[k] + 0x8000) >> 16) - (long)(b[k] >> 16));
            worst = diff > worst ? diff : worst;
            sum += (double)diff * diff;
        }
        double rms = na > 0 ? sqrt(sum / ((double)na * fa.channels)) : 99;
        CHECK(ea == 0 && eb == 0 && na == nb && worst <= 1 && rms < 0.5, "vorbis %s: %ld/%ld frames, error %d, %ld, rms %.3f",
              names[i], na, nb, ea, worst, rms);
        free(a);
        free(b);
    }
}

int main(void)
{
    test_md5();
    test_imdct();
    test_flac();
    test_vorbis();
    printf("libcodec tests: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
