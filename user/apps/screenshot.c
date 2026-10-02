/* screenshot: save the screen as a PNG file through the screencopy
 * interface of the display server.
 *
 *   screenshot [-d SECONDS] [FILE]
 *
 * Without FILE the image is written to $HOME/Pictures/screenshot-DATE-
 * TIME.png, and the directory is created when it does not exist. The
 * image has the device resolution of the screen. The path of the file
 * is printed on standard output. The display server starts this program
 * on the Print Screen key. */
#include <gui/client.h>
#include <gui/image.h>
#include <wire/client.h>
#include <core-client.h>
#include <debug-client.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FORMAT_XRGB8888 1

static int width, height, done, failed;

static void on_size(void *user, struct wire_proxy *self, int32_t w, int32_t h, int32_t s)
{
    width = w;
    height = h;
}
static void on_done(void *user, struct wire_proxy *self) { done = 1; }
static void on_failed(void *user, struct wire_proxy *self) { failed = 1; }
static const struct screencopy_listener screencopy_events = { on_size, on_done, on_failed };

/* $HOME/Pictures/screenshot-YYYY-MM-DD-HHMMSS.png, with -2, -3 and so on
 * appended when a file of that name exists. */
static int default_path(char *path, size_t size)
{
    const char *home = getenv("HOME");
    char dir[200];
    snprintf(dir, sizeof dir, "%s/Pictures", home && home[0] ? home : "/home");
    if (mkdir(dir, 0755) < 0 && errno != EEXIST)
        return -errno;
    time_t now = time(NULL);
    struct tm tm;
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d-%H%M%S", gmtime_r(&now, &tm));
    struct stat st;
    for (int n = 1; n < 100; n++) {
        if (n == 1)
            snprintf(path, size, "%s/screenshot-%s.png", dir, stamp);
        else
            snprintf(path, size, "%s/screenshot-%s-%d.png", dir, stamp, n);
        if (stat(path, &st) < 0)
            return 0;
    }
    return -EEXIST;
}

static void usage(void)
{
    fprintf(stderr, "usage: screenshot [-d SECONDS] [FILE]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int delay = 0;
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            delay = atoi(argv[++i]);
        else if (argv[i][0] == '-')
            usage();
        else if (!file)
            file = argv[i];
        else
            usage();
    }
    char path[256];
    if (file) {
        strlcpy(path, file, sizeof path);
    } else {
        int err = default_path(path, sizeof path);
        if (err < 0) {
            fprintf(stderr, "screenshot: no file name: %s\n", strerror(-err));
            return 1;
        }
    }
    if (delay > 0)
        sleep((unsigned)delay);
    if (gui_connect() < 0) {
        fprintf(stderr, "screenshot: cannot connect to the display server: %s\n", strerror(errno));
        return 1;
    }
    struct wire_display *d = gui_display();
    struct wire_proxy *sc = gui_bind_global("screencopy", &screencopy_interface, 1);
    struct wire_proxy *shm = gui_bind_global("shm", &shm_interface, 1);
    if (!sc || !shm) {
        fprintf(stderr, "screenshot: the display server has no screencopy interface\n");
        return 1;
    }
    screencopy_add_listener(sc, &screencopy_events, NULL);
    wire_display_roundtrip(d);
    if (width <= 0 || height <= 0) {
        fprintf(stderr, "screenshot: no screen size from the display server\n");
        return 1;
    }
    size_t bytes = (size_t)width * height * 4;
    int fd = memfd_create("screenshot", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (long)bytes) < 0) {
        fprintf(stderr, "screenshot: cannot allocate %zu bytes: %s\n", bytes, strerror(errno));
        return 1;
    }
    uint32_t *pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        fprintf(stderr, "screenshot: cannot map %zu bytes: %s\n", bytes, strerror(errno));
        return 1;
    }
    struct wire_proxy *pool = shm_create_pool(shm, fd, (int32_t)bytes);
    struct wire_proxy *buffer = shm_pool_create_buffer(pool, 0, width, height, width * 4, FORMAT_XRGB8888);
    screencopy_capture(sc, buffer);
    while (!done && !failed && wire_display_error(d) == 0)
        if (wire_display_roundtrip(d) < 0)
            break;
    if (!done) {
        fprintf(stderr, "screenshot: the display server did not copy the screen\n");
        return 1;
    }
    struct image img = { width, height, pixels, 1 };
    int err = image_save_png(&img, path);
    buffer_destroy(buffer);
    shm_pool_destroy(pool);
    screencopy_destroy(sc);
    wire_display_roundtrip(d);
    munmap(pixels, bytes);
    close(fd);
    gui_disconnect();
    if (err < 0) {
        fprintf(stderr, "screenshot: %s: %s\n", path, strerror(-err));
        return 1;
    }
    printf("%s\n", path);
    return 0;
}
