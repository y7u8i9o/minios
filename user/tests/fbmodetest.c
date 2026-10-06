/* fbmodetest: change the display mode through /dev/fb0, draw, flush and
 * restore the boot mode. It then draws three squares and flushes two of
 * them with FBIO_FLUSH_RECTS, which a screendump of the gpu_mode case
 * checks on the host. Driven by the gpu_mode kernel test. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <minios/abi.h>

int main(void)
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        printf("fbmodetest: open: %s\n", strerror(errno));
        return 1;
    }
    struct fb_info info;
    if (ioctl(fd, FBIOGET_INFO, &info) < 0 || !(info.caps & FB_CAP_SET_MODE)) {
        printf("fbmodetest: no mode setting\n");
        return 2;
    }
    uint32_t *fb = mmap(NULL, info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        printf("fbmodetest: mmap: %s\n", strerror(errno));
        return 1;
    }
    struct fb_mode mode = { 1280, 800, 1 };
    if (ioctl(fd, FBIO_SET_MODE, &mode) == 0 || errno != EPERM) {
        printf("fbmodetest: mode change without owning the display not refused\n");
        return 1;
    }
    if (ioctl(fd, FBIO_ACQUIRE, 0) < 0 || ioctl(fd, FBIO_SET_MODE, &mode) < 0 || ioctl(fd, FBIOGET_INFO, &info) < 0) {
        printf("fbmodetest: set mode: %s\n", strerror(errno));
        return 1;
    }
    printf("fbmodetest: mode %ux%u pitch %u scale %u\n", info.width, info.height, info.pitch, info.scale);
    uint32_t stride = info.pitch / 4;
    for (uint32_t y = 0; y < info.height; y++)
        for (uint32_t x = 0; x < info.width; x++)
            fb[y * stride + x] = 0x00336699;
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++)
            fb[(y + 100) * stride + x + 100] = ((x ^ y) & 1) ? 0x00ff8800 : 0x000044ff;
    struct fb_rect all = { 0, 0, (int32_t)info.width, (int32_t)info.height };
    if (ioctl(fd, FBIO_FLUSH, &all) < 0) {
        printf("fbmodetest: flush: %s\n", strerror(errno));
        return 1;
    }
    if (!(info.caps & FB_CAP_FLUSH_RECTS)) {
        printf("fbmodetest: no FB_CAP_FLUSH_RECTS\n");
        return 1;
    }
    /* Red, green and blue squares of 64 pixels at x 300, 500 and 700 and
     * y 300. One call flushes the red and the green square. The host
     * shows the blue square only after a later flush. */
    static const uint32_t colors[3] = { 0x00ff0000, 0x0000ff00, 0x000000ff };
    for (int k = 0; k < 3; k++)
        for (uint32_t y = 300; y < 364; y++)
            for (uint32_t x = 300 + 200 * k; x < 364 + 200 * k; x++)
                fb[y * stride + x] = colors[k];
    struct fb_flush_rects two = { 2, 0, { { 300, 300, 64, 64 }, { 500, 300, 64, 64 } } };
    if (ioctl(fd, FBIO_FLUSH_RECTS, &two) < 0) {
        printf("fbmodetest: FBIO_FLUSH_RECTS: %s\n", strerror(errno));
        return 1;
    }
    struct fb_flush_rects many = { FB_FLUSH_MAX + 1, 0, { { 0, 0, 1, 1 } } };
    if (ioctl(fd, FBIO_FLUSH_RECTS, &many) == 0 || errno != EINVAL) {
        printf("fbmodetest: a count above FB_FLUSH_MAX was not refused\n");
        return 1;
    }
    /* A second open file that does not own the display. */
    pid_t child = fork();
    if (child == 0) {
        int other = open("/dev/fb0", O_RDWR);
        int r = other >= 0 ? ioctl(other, FBIO_FLUSH_RECTS, &two) : 0;
        _exit(other >= 0 && r < 0 && errno == EPERM ? 0 : 1);
    }
    int status = 1;
    if (child < 0 || waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("fbmodetest: a file that does not own the display was not refused\n");
        return 1;
    }
    printf("fbmodetest: flushed two of three squares\n");
    fflush(stdout);
    int m = open("/fb.ready", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (m >= 0)
        close(m);
    char line[16];
    read(0, line, sizeof line);
    struct fb_mode boot = { 1024, 768, 1 };
    if (ioctl(fd, FBIO_SET_MODE, &boot) < 0) {
        printf("fbmodetest: restore: %s\n", strerror(errno));
        return 1;
    }
    munmap(fb, info.size);
    ioctl(fd, FBIO_RELEASE, 0);
    close(fd);
    return 0;
}
