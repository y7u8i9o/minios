#pragma once
#include <kernel.h>
#include <arch/boot.h>
#include <minios/abi.h>

/* The active display. fb_screen starts as the Limine framebuffer (fb_screen_init)
 * and is replaced by the GPU driver's buffer (fb_gpu_register); its
 * geometry changes in fb_set_mode. Every change happens under console_lock
 * (console_set_screen), the console reads it under that lock, /dev/fb0
 * reads it under fb_mode_lock and the tests read it from one thread. The
 * address never changes after boot. */
extern struct limine_framebuffer fb_screen;
extern bool fb_screen_present;
extern uint32_t fb_screen_scale;         /* pixels per logical pixel, 1..4 */
void fb_screen_init(void);

/* /dev/fb0: the active framebuffer for user space. */
void fbdev_init(void);

/* A GPU driver whose buffer needs explicit flushes and which can change
 * the mode. All operations may sleep except flush_poll. */
struct fb_gpu_ops {
    /* Create the device side resource for a mode; fb_screen is untouched. */
    int (*prepare_mode)(void *priv, uint32_t width, uint32_t height);
    /* Scan out the prepared resource and drop the previous one. */
    int (*commit_mode)(void *priv);
    int (*flush)(void *priv, struct fb_rect r);
    void (*flush_poll)(void *priv, struct fb_rect r);   /* panic path */
};
/* Called once at boot by the GPU driver after prepare_mode for the boot
 * mode: switches the console to screen (a 32 bpp buffer in the direct
 * map of size bytes), commits the mode and flushes. */
void fb_gpu_register(const struct fb_gpu_ops *ops, void *priv,
                     const struct limine_framebuffer *screen, size_t size);
bool fb_has_gpu(void);
size_t fb_map_size(void);
/* Push a rectangle to the display; 0 without a GPU. */
int fb_flush(struct fb_rect r);
/* Change the resolution (GPU only) and the pixel scale; the console
 * follows. Returns -EOPNOTSUPP without a GPU unless only the scale changes. */
int fb_set_mode(uint32_t width, uint32_t height, uint32_t scale);
/* Best effort flush of the whole screen without interrupts. */
void fb_panic_flush(void);

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
