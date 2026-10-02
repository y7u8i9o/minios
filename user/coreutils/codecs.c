/* codecs: the codec modules of /lib/codecs (docs/design/codecs.md), in
 * the manner of gst-inspect and ffmpeg.
 *
 *   codecs                       list the codecs and their modules
 *   codecs info FILE...          name the format of each file and describe it
 *   codecs convert [-f NAME] [-b BITS] [-s SIZE] IN OUT
 *                                convert IN to the format of OUT's extension,
 *                                or to the codec NAME
 *
 * -b gives the sample size of converted audio (the source's by default),
 * -s the size at which a vector image is rendered (its own by default).
 * Exit status 0 on success, 1 on an error, 2 for wrong usage. */
#include <codec/codec.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static __attribute__((noreturn)) void usage(void)
{
    fprintf(stderr, "usage: codecs\n"
                    "       codecs info FILE...\n"
                    "       codecs convert [-f NAME] [-b BITS] [-s SIZE] IN OUT\n");
    exit(2);
}

static const char *kind_name(enum codec_kind k)
{
    return k == CODEC_IMAGE ? "image" : k == CODEC_AUDIO ? "audio" : "?";
}

/* D decodes, E encodes, S scales (renders at any size). */
static const char *caps_text(int caps, char out[4])
{
    out[0] = caps & CODEC_DECODE ? 'D' : '-';
    out[1] = caps & CODEC_ENCODE ? 'E' : '-';
    out[2] = caps & CODEC_SCALABLE ? 'S' : '-';
    out[3] = '\0';
    return out;
}

static const char *module_of(const struct codec *c)
{
    for (int m = 0; m < codec_module_count(); m++) {
        const struct codec_module *mod = codec_module_get(m);
        if (c >= mod->codecs && c < mod->codecs + mod->count) {
            const char *path = codec_module_path(m), *slash = strrchr(path, '/');
            return slash ? slash + 1 : path;
        }
    }
    return "?";
}

static int list(void)
{
    int n = codec_count();
    for (int i = 0; i < n; i++) {
        const struct codec *c = codec_get(i);
        char caps[4];
        printf("%s %-5s %s  %-8s %s\n", caps_text(c->caps, caps), c->name, kind_name(c->kind), module_of(c),
               c->description);
        printf("            types %s, extensions %s\n", c->mime_types, c->extensions);
    }
    printf("%d codecs in %d modules\n", n, codec_module_count());
    return 0;
}

static int info(const char *path)
{
    uint8_t *data;
    size_t len;
    int err = codec_read_file(path, &data, &len);
    if (err < 0) {
        fprintf(stderr, "codecs: %s: %s\n", path, strerror(-err));
        return 1;
    }
    const struct codec *c = codec_identify(0, data, len, path, CODEC_DECODE);
    int status = 0;
    if (!c) {
        printf("%s: unknown format\n", path);
        status = 1;
    } else if (c->kind == CODEC_IMAGE) {
        struct codec_picture pic;
        err = codec_image_decode(c, data, len, path, NULL, &pic);
        if (err < 0) {
            printf("%s: %s image, invalid: %s\n", path, c->name, strerror(-err));
            status = 1;
        } else {
            int alpha = 0;
            for (size_t i = 0, n = (size_t)pic.w * pic.h; i < n && !alpha; i++)
                alpha = pic.pixels[i] >> 24 != 0xff;
            printf("%s: %s image, %dx%d%s%s\n", path, c->name, pic.w, pic.h, alpha ? ", with alpha" : "",
                   c->caps & CODEC_SCALABLE ? ", scalable" : "");
            codec_picture_free(&pic);
        }
    } else {
        struct codec_audio *a;
        err = codec_audio_open(c, data, len, path, &a);
        if (err < 0) {
            printf("%s: %s audio, invalid: %s\n", path, c->name, strerror(-err));
            status = 1;
        } else {
            const struct codec_audio_format *f = codec_audio_format(a);
            long frames = codec_audio_frames(a);
            long ms = frames >= 0 && f->rate ? (long)((long long)frames * 1000 / f->rate) : -1;
            printf("%s: %s audio, %d Hz, %d channel%s, %d bit, %ld frames, %ld.%03ld s\n", path, c->name, f->rate,
                   f->channels, f->channels == 1 ? "" : "s", f->bits, frames, ms / 1000, ms % 1000);
            codec_audio_close(a);
        }
    }
    free(data);
    return status;
}

static int fail(const char *what, const char *path, int err)
{
    fprintf(stderr, "codecs: %s: %s%s%s\n", path, what, err ? ": " : "", err ? strerror(-err) : "");
    return 1;
}

static int convert_image(const struct codec *from, const struct codec *to, const uint8_t *data, size_t len,
                         const char *in, const char *out, int size)
{
    struct codec_image_request req = { size, size, 0 };
    struct codec_picture pic;
    int err = codec_image_decode(from, data, len, in, &req, &pic);
    if (err < 0)
        return fail("cannot decode", in, err);
    uint8_t *file;
    long n = codec_image_encode(to, &pic, &file);
    codec_picture_free(&pic);
    if (n < 0)
        return fail("cannot encode", out, (int)n);
    err = codec_write_file(out, file, (size_t)n);
    free(file);
    return err < 0 ? fail("cannot write", out, err) : 0;
}

static int convert_audio(const struct codec *from, const struct codec *to, const uint8_t *data, size_t len,
                         const char *in, const char *out, int bits)
{
    struct codec_audio *a;
    int err = codec_audio_open(from, data, len, in, &a);
    if (err < 0)
        return fail("cannot decode", in, err);
    struct codec_audio_format fmt = *codec_audio_format(a);
    long cap = codec_audio_frames(a) > 0 ? codec_audio_frames(a) : 4096, n = 0, got;
    int32_t *samples = malloc((size_t)cap * fmt.channels * sizeof *samples);
    while (samples && (got = codec_audio_read(a, samples + n * fmt.channels, cap - n)) > 0) {
        n += got;
        if (n == cap) {
            int32_t *grown = realloc(samples, (size_t)cap * 2 * fmt.channels * sizeof *samples);
            if (!grown) {
                free(samples);
                samples = NULL;
                break;
            }
            samples = grown;
            cap *= 2;
        }
    }
    codec_audio_close(a);
    if (!samples)
        return fail("cannot decode", in, -ENOMEM);
    if (bits)
        fmt.bits = bits;
    uint8_t *file;
    long size = codec_audio_encode(to, &fmt, samples, n, &file);
    free(samples);
    if (size < 0)
        return fail("cannot encode", out, (int)size);
    err = codec_write_file(out, file, (size_t)size);
    free(file);
    return err < 0 ? fail("cannot write", out, err) : 0;
}

static int convert(int argc, char **argv)
{
    const char *name = NULL;
    int bits = 0, size = 0, opt;
    optind = 1;
    while ((opt = getopt(argc, argv, "f:b:s:")) != -1) {
        switch (opt) {
        case 'f': name = optarg; break;
        case 'b': bits = atoi(optarg); break;
        case 's': size = atoi(optarg); break;
        default: usage();
        }
    }
    if (argc - optind != 2)
        usage();
    const char *in = argv[optind], *out = argv[optind + 1];
    uint8_t *data;
    size_t len;
    int err = codec_read_file(in, &data, &len);
    if (err < 0)
        return fail("", in, err);
    const struct codec *from = codec_identify(0, data, len, in, CODEC_DECODE);
    const struct codec *to = name ? codec_find(name) : from ? codec_for_path(from->kind, out, CODEC_ENCODE) : NULL;
    int status;
    if (!from)
        status = fail("unknown format", in, 0);
    else if (!to || !(to->caps & CODEC_ENCODE))
        status = fail(name ? "no encoder of that name" : "no encoder for the extension", out, 0);
    else if (to->kind != from->kind)
        status = fail("the formats are of different kinds", out, 0);
    else if (from->kind == CODEC_IMAGE)
        status = convert_image(from, to, data, len, in, out, size);
    else
        status = convert_audio(from, to, data, len, in, out, bits);
    if (!status)
        printf("%s (%s) -> %s (%s)\n", in, from->name, out, to->name);
    free(data);
    return status;
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return list();
    if (strcmp(argv[1], "info") == 0) {
        if (argc < 3)
            usage();
        int status = 0;
        for (int i = 2; i < argc; i++)
            status |= info(argv[i]);
        return status;
    }
    if (strcmp(argv[1], "convert") == 0)
        return convert(argc - 1, argv + 1);
    usage();
}
