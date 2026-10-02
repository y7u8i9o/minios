/* The codec library and its modules in /lib/codecs (docs/design/codecs.md).
 * "codectest image" checks the registry, probing and lookups, image round
 * trips through the modules and through libgui, errors, and CODEC_PATH;
 * "codectest audio" checks the audio streams and the WAV module. Other
 * arguments run both. "codectest count" prints the number of codecs, for
 * the child started with another CODEC_PATH. */
#include <codec/codec.h>
#include <gui/image.h>
#include <errno.h>
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
        /* A cut in the middle of the last sample keeps the whole frames. */
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
    int image = argc < 2 || strcmp(argv[1], "audio") != 0;
    int audio = argc < 2 || strcmp(argv[1], "image") != 0;
    list_registry();
    if (image) {
        test_registry();
        test_probe();
        test_images();
        test_path();
    }
    if (audio)
        test_audio();
    printf("codectest: %d failures\n", failures);
    return failures ? 1 : 0;
}
