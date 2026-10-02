/* The codec library and its modules in /lib/codecs (docs/design/codecs.md):
 * the registry, probing and lookups, image round trips through the
 * modules and through libgui, errors, and CODEC_PATH. "codectest count"
 * prints the number of codecs, for the child started with another
 * CODEC_PATH. */
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
    int modules = codec_module_count();
    printf("codectest: %d modules, %d codecs\n", modules, codec_count());
    for (int i = 0; i < modules; i++)
        printf("codectest: module %s from %s\n", codec_module_get(i)->name, codec_module_path(i));
    const struct codec *png = codec_find("png"), *svg = codec_find("SVG");
    CHECK(png && png->kind == CODEC_IMAGE && png->caps == (CODEC_DECODE | CODEC_ENCODE), "png codec");
    CHECK(svg && svg->kind == CODEC_IMAGE && svg->caps == CODEC_DECODE, "svg codec, found without regard to case");
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

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "count") == 0) {
        printf("%d\n", codec_count());
        return 0;
    }
    test_registry();
    test_probe();
    test_images();
    test_path();
    printf("codectest: %d failures\n", failures);
    return failures ? 1 : 0;
}
