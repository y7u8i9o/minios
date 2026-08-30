#pragma once
#include <kernel.h>
#include <arch/boot.h>

/* /dev/fb0: the Limine framebuffer for user space. */
void fbdev_init(void);

/* Pixel layout helpers shared by the console, the device and the tests.
 * Colors are 0x00RRGGBB; pixels are in the framebuffer's own layout,
 * 24 or 32 bits wide. */
static inline uint32_t fb_pack_pixel(const struct limine_framebuffer *fb, uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    return ((r >> (8 - fb->red_mask_size)) << fb->red_mask_shift) |
           ((g >> (8 - fb->green_mask_size)) << fb->green_mask_shift) |
           ((b >> (8 - fb->blue_mask_size)) << fb->blue_mask_shift);
}

static inline uint32_t fb_unpack_pixel(const struct limine_framebuffer *fb, uint32_t pix)
{
    uint32_t r = (pix >> fb->red_mask_shift) & ((1u << fb->red_mask_size) - 1);
    uint32_t g = (pix >> fb->green_mask_shift) & ((1u << fb->green_mask_size) - 1);
    uint32_t b = (pix >> fb->blue_mask_shift) & ((1u << fb->blue_mask_size) - 1);
    return ((r << (8 - fb->red_mask_size)) << 16) | ((g << (8 - fb->green_mask_size)) << 8) |
           (b << (8 - fb->blue_mask_size));
}

/* True for the fast path: 32 bits per pixel with 0x00RRGGBB layout. */
static inline bool fb_is_native_rgb32(const struct limine_framebuffer *fb)
{
    return fb->bpp == 32 && fb->red_mask_size == 8 && fb->red_mask_shift == 16 &&
           fb->green_mask_size == 8 && fb->green_mask_shift == 8 &&
           fb->blue_mask_size == 8 && fb->blue_mask_shift == 0;
}

static inline void fb_write_pixel(const struct limine_framebuffer *fb, uint32_t x, uint32_t y, uint32_t pix)
{
    volatile uint8_t *p = (volatile uint8_t *)fb->address + y * fb->pitch + x * (fb->bpp / 8);
    if (fb->bpp == 32) {
        *(volatile uint32_t *)p = pix;
    } else {
        p[0] = (uint8_t)pix;
        p[1] = (uint8_t)(pix >> 8);
        p[2] = (uint8_t)(pix >> 16);
    }
}

static inline uint32_t fb_read_pixel(const struct limine_framebuffer *fb, uint32_t x, uint32_t y)
{
    volatile uint8_t *p = (volatile uint8_t *)fb->address + y * fb->pitch + x * (fb->bpp / 8);
    if (fb->bpp == 32)
        return *(volatile uint32_t *)p;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

/* Read a pixel as 0x00RRGGBB (tests). */
static inline uint32_t fb_read_rgb(const struct limine_framebuffer *fb, uint32_t x, uint32_t y)
{
    return fb_unpack_pixel(fb, fb_read_pixel(fb, x, y));
}
