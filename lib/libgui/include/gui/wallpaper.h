#pragma once
/* The background of the desktop and of the greeter: a colour and an
 * optional image in one of four modes (docs/design/desktop.md). */
#include <stdint.h>
#include <gui/image.h>

enum wallpaper_mode {
    WALLPAPER_FILL,             /* cover the area, cut the longer side */
    WALLPAPER_CENTER,           /* the image at its size in the middle */
    WALLPAPER_TILE,             /* the image repeated from the top left corner */
    WALLPAPER_STRETCH,          /* the image scaled to the area */
};

/* The mode of the text fill, center, tile or stretch. Any other text gives
 * WALLPAPER_FILL. */
enum wallpaper_mode wallpaper_mode_parse(const char *text);

/* The background of an area of w by h logical pixels at scale device
 * pixels per logical pixel: an opaque image of w * scale by h * scale
 * pixels with that scale, which painter_image copies pixel for pixel. The
 * area is color (0x00rrggbb) with img drawn over it in mode, or color
 * alone when img is NULL. Center and tile give each pixel of img its
 * logical size, the way the painter draws images. image_scale scales img,
 * so the result has the full resolution of the device. NULL with errno. */
struct image *wallpaper_render(const struct image *img, enum wallpaper_mode mode, uint32_t color, int w, int h,
                               int scale);
