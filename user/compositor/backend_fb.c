/* Framebuffer backend: the device mapping, a 32 bit back buffer and the
 * copy with format conversion (from the M19 window server). */
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
static int fb_fd = -1;
static uint8_t *fbmem;
static struct fb_info fbinfo;
static int fb_native;

int backend_init(void)
{
    fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_INFO, &fbinfo) < 0)
        return -1;
    if (fbinfo.bpp != 32 && fbinfo.bpp != 24)
        return -1;
    fbmem = mmap(NULL, (size_t)fbinfo.pitch * fbinfo.height, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fbmem == MAP_FAILED)
        return -1;
    screen_w = (int)fbinfo.width;
    screen_h = (int)fbinfo.height;
    fb_native = fbinfo.bpp == 32 && fbinfo.red_size == 8 && fbinfo.red_shift == 16 &&
                fbinfo.green_size == 8 && fbinfo.green_shift == 8 && fbinfo.blue_size == 8 && fbinfo.blue_shift == 0;
    back.width = screen_w;
    back.height = screen_h;
    back.stride = screen_w;
    back.pixels = malloc((size_t)screen_w * screen_h * 4);
    if (!back.pixels)
        return -1;
    if (ioctl(fb_fd, FBIO_ACQUIRE, 0) < 0)
        return -1;
    return 0;
}

void backend_flush(struct rect r)
{
    for (int j = 0; j < r.h; j++) {
        const uint32_t *from = back.pixels + (size_t)(r.y + j) * back.stride + r.x;
        uint8_t *to = fbmem + (size_t)(r.y + j) * fbinfo.pitch + (size_t)r.x * (fbinfo.bpp / 8);
        if (fb_native) {
            memcpy(to, from, (size_t)r.w * 4);
            continue;
        }
        for (int i = 0; i < r.w; i++) {
            uint32_t c = from[i];
            uint32_t pix = (((c >> 16) & 0xff) >> (8 - fbinfo.red_size)) << fbinfo.red_shift |
                           (((c >> 8) & 0xff) >> (8 - fbinfo.green_size)) << fbinfo.green_shift |
                           ((c & 0xff) >> (8 - fbinfo.blue_size)) << fbinfo.blue_shift;
            if (fbinfo.bpp == 32) {
                *(uint32_t *)(to + i * 4) = pix;
            } else {
                to[i * 3] = (uint8_t)pix;
                to[i * 3 + 1] = (uint8_t)(pix >> 8);
                to[i * 3 + 2] = (uint8_t)(pix >> 16);
            }
        }
    }
}

void backend_release(void)
{
    if (fb_fd >= 0)
        ioctl(fb_fd, FBIO_RELEASE, 0);
}
