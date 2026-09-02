/* Framebuffer backend: the device mapping, a 32 bit back buffer and the
 * copy with format conversion (from the M19 window server).
 *
 * The back buffer and everything above it use logical pixels. When the
 * mode was chosen with video=WxH@N (high density displays), /dev/fb0
 * reports scale N and the screen is width/N by height/N logical pixels;
 * the flush writes every logical pixel as an N by N block, so windows
 * and text keep their size on screen and stay crisp. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <minios/abi.h>
#include "comp.h"

struct surface back;
int screen_w, screen_h;
int screen_scale = 1;
static int fb_fd = -1;
static uint8_t *fbmem;
static struct fb_info fbinfo;
static int fb_native;
static uint8_t *line;                   /* one framebuffer row for scaled flushes */
static size_t map_size;

/* Take the geometry from the device and size the buffers. */
static int backend_setup(void)
{
    if (ioctl(fb_fd, FBIOGET_INFO, &fbinfo) < 0)
        return -1;
    if (fbinfo.bpp != 32 && fbinfo.bpp != 24)
        return -1;
    screen_scale = fbinfo.scale >= 1 && fbinfo.scale <= 4 ? fbinfo.scale : 1;
    if (fbinfo.width / (uint32_t)screen_scale < 640 || fbinfo.height / (uint32_t)screen_scale < 480)
        screen_scale = 1;
    screen_w = (int)(fbinfo.width / (uint32_t)screen_scale);
    screen_h = (int)(fbinfo.height / (uint32_t)screen_scale);
    fb_native = fbinfo.bpp == 32 && fbinfo.red_size == 8 && fbinfo.red_shift == 16 &&
                fbinfo.green_size == 8 && fbinfo.green_shift == 8 && fbinfo.blue_size == 8 && fbinfo.blue_shift == 0;
    free(back.pixels);
    free(line);
    back.width = screen_w;
    back.height = screen_h;
    back.stride = screen_w;
    back.pixels = malloc((size_t)screen_w * screen_h * 4);
    line = malloc((size_t)fbinfo.width * 4);
    return back.pixels && line ? 0 : -1;
}

int backend_init(void)
{
    fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_INFO, &fbinfo) < 0)
        return -1;
    /* With mode setting the whole buffer is mapped once; the mapping
     * stays valid across mode changes. */
    map_size = (fbinfo.caps & FB_CAP_SET_MODE) ? fbinfo.size : (size_t)fbinfo.pitch * fbinfo.height;
    fbmem = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fbmem == MAP_FAILED)
        return -1;
    if (backend_setup() < 0)
        return -1;
    if (ioctl(fb_fd, FBIO_ACQUIRE, 0) < 0)
        return -1;
    return 0;
}

int backend_can_set_mode(void)
{
    return (fbinfo.caps & FB_CAP_SET_MODE) != 0;
}

int backend_set_mode(int width, int height, int scale)
{
    struct fb_mode m = { (uint32_t)width, (uint32_t)height, (uint32_t)scale };
    if (ioctl(fb_fd, FBIO_SET_MODE, &m) < 0)
        return -1;
    return backend_setup();
}

static inline uint32_t pack(uint32_t c)
{
    return (((c >> 16) & 0xff) >> (8 - fbinfo.red_size)) << fbinfo.red_shift |
           (((c >> 8) & 0xff) >> (8 - fbinfo.green_size)) << fbinfo.green_shift |
           ((c & 0xff) >> (8 - fbinfo.blue_size)) << fbinfo.blue_shift;
}

void backend_flush(struct rect r)
{
    int s = screen_scale;
    size_t bpp = fbinfo.bpp / 8;
    for (int j = 0; j < r.h; j++) {
        const uint32_t *from = back.pixels + (size_t)(r.y + j) * back.stride + r.x;
        uint8_t *to = fbmem + (size_t)(r.y + j) * s * fbinfo.pitch + (size_t)r.x * (size_t)s * bpp;
        if (fb_native && s == 1) {
            memcpy(to, from, (size_t)r.w * 4);
            continue;
        }
        /* Expand the row once, then copy it to each of the s rows. */
        size_t bytes = (size_t)r.w * (size_t)s * bpp;
        if (fbinfo.bpp == 32) {
            uint32_t *out = (uint32_t *)line;
            for (int i = 0; i < r.w; i++) {
                uint32_t pix = fb_native ? from[i] : pack(from[i]);
                for (int k = 0; k < s; k++)
                    *out++ = pix;
            }
        } else {
            uint8_t *out = line;
            for (int i = 0; i < r.w; i++) {
                uint32_t pix = pack(from[i]);
                for (int k = 0; k < s; k++) {
                    *out++ = (uint8_t)pix;
                    *out++ = (uint8_t)(pix >> 8);
                    *out++ = (uint8_t)(pix >> 16);
                }
            }
        }
        for (int k = 0; k < s; k++)
            memcpy(to + (size_t)k * fbinfo.pitch, line, bytes);
    }
    if (fbinfo.caps & FB_CAP_FLUSH) {
        struct fb_rect fr = { r.x * s, r.y * s, r.w * s, r.h * s };
        ioctl(fb_fd, FBIO_FLUSH, &fr);
    }
}

void backend_release(void)
{
    if (fb_fd >= 0)
        ioctl(fb_fd, FBIO_RELEASE, 0);
}
