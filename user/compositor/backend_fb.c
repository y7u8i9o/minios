/* Framebuffer backend: the device mapping, a 32 bit back buffer and the
 * copy with format conversion (from the M19 window server).
 *
 * The back buffer contains device pixels; the scene composes into it at
 * screen_scale device pixels per logical pixel (video=WxH@N, high
 * density displays), so scaled client buffers and decorations are sharp.
 * The scene, the shell and input work in logical pixels: width/N by
 * height/N.
 *
 * A device that needs flushes (virtio-gpu) shows nothing until a flush,
 * and its mapping is ordinary RAM. When its format is the native
 * 0x00RRGGBB word, the scene composes directly into the mapping, and a
 * frame needs no copy (G5 of docs/plan/compositor-performance.md).
 * Otherwise (std VGA, ramfb, other formats) the display scans the mapping
 * continuously, so the scene composes into a private back buffer and the
 * present copies the finished rectangles. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <minios/abi.h>
#include <gui/pixel.h>
#include "comp.h"

struct surface back;
int screen_w, screen_h;
int screen_scale = 1;
static int fb_fd = -1;
static uint8_t *fbmem;
static struct fb_info fbinfo;
static int fb_native;
static int direct;                      /* back.pixels is the framebuffer mapping */
static struct pixel_format fb_format;   /* the device format when it is not native */
static uint32_t *own_back;              /* the private back buffer, NULL with direct */
static uint8_t *line;                   /* one framebuffer row in the device format */
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
    fb_format = (struct pixel_format){ (int)fbinfo.bpp / 8, fbinfo.red_size, fbinfo.red_shift, fbinfo.green_size,
                                       fbinfo.green_shift, fbinfo.blue_size, fbinfo.blue_shift };
    free(own_back);
    free(line);
    own_back = NULL;
    line = NULL;
    back.width = screen_w * screen_scale;
    back.height = screen_h * screen_scale;
    direct = (fbinfo.caps & FB_CAP_FLUSH) && fb_native && fbinfo.pitch % 4 == 0;
    if (direct) {
        back.pixels = (uint32_t *)fbmem;
        back.stride = (int)(fbinfo.pitch / 4);
        return 0;
    }
    back.stride = back.width;
    back.pixels = own_back = malloc((size_t)back.width * back.height * 4);
    line = malloc((size_t)fbinfo.width * 4);
    return own_back && line ? 0 : -1;
}

int backend_init(void)
{
    fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_INFO, &fbinfo) < 0)
        return -1;
    /* With mode setting the whole buffer is mapped once; the mapping
     * remains valid across mode changes. */
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

int backend_display_fd(void)
{
    return fb_fd;
}

int backend_display_request(int *width, int *height)
{
    struct fb_display d;
    if (ioctl(fb_fd, FBIOGET_DISPLAY, &d) < 0 || d.serial == 0 || d.width == 0 || d.height == 0)
        return -1;
    *width = (int)d.width;
    *height = (int)d.height;
    return 0;
}

void backend_present(const struct rect *r, int n)
{
    int s = screen_scale;
    size_t bpp = fbinfo.bpp / 8;
    long t0 = uptime_us(), bytes = 0;
    struct fb_flush_rects fr = { 0, 0, { { 0, 0, 0, 0 } } };
    for (int i = 0; i < n && fr.count < FB_FLUSH_MAX; i++) {
        /* Logical rectangle to device rows, clipped to the back buffer. */
        struct rect d = rect_intersect(rect_scale(r[i], s), (struct rect){ 0, 0, back.width, back.height });
        if (rect_empty(d))
            continue;
        for (int j = 0; j < d.h && !direct; j++) {
            const uint32_t *from = back.pixels + (size_t)(d.y + j) * back.stride + d.x;
            uint8_t *to = fbmem + (size_t)(d.y + j) * fbinfo.pitch + (size_t)d.x * bpp;
            if (fb_native) {
                memcpy(to, from, (size_t)d.w * 4);
                continue;
            }
            pixel_pack(line, from, d.w, &fb_format);
            memcpy(to, line, (size_t)d.w * bpp);
        }
        bytes += (long)d.w * d.h * (long)bpp;
        fr.rects[fr.count++] = (struct fb_rect){ d.x, d.y, d.w, d.h };
    }
    if (fr.count && (fbinfo.caps & FB_CAP_FLUSH_RECTS)) {
        ioctl(fb_fd, FBIO_FLUSH_RECTS, &fr);
    } else if (fbinfo.caps & FB_CAP_FLUSH) {
        for (uint32_t i = 0; i < fr.count; i++)
            ioctl(fb_fd, FBIO_FLUSH, &fr.rects[i]);
    }
    stats_flush((long)fr.count, bytes, uptime_us() - t0);
}

long backend_buffer_bytes(void)
{
    return own_back ? (long)back.width * back.height * 4 : 0;
}

void backend_release(void)
{
    if (fb_fd >= 0)
        ioctl(fb_fd, FBIO_RELEASE, 0);
}
