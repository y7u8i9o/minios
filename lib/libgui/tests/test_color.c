/* Colour conversions and display modes (X1 of docs/plan/x12settings.md). */
#include <gui/gfx.h>
#include <gui/display.h>
#include <string.h>
#include "check.h"

static int channel_error(uint32_t a, uint32_t b)
{
    int worst = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        int d = (int)(a >> sh & 0xff) - (int)(b >> sh & 0xff);
        if (d < 0)
            d = -d;
        if (d > worst)
            worst = d;
    }
    return worst;
}

void run_color_tests(void)
{
    int worst = 0;
    uint32_t worst_rgb = 0;
    for (uint32_t rgb = 0; rgb < 0x1000000; rgb++) {
        int h, s, v;
        gfx_rgb_to_hsv(rgb, &h, &s, &v);
        int e = channel_error(rgb, gfx_hsv_to_rgb(h, s, v));
        if (e > worst) {
            worst = e;
            worst_rgb = rgb;
        }
    }
    CHECK(worst <= 3, "HSV round trip of every colour: largest channel error %d at %06x", worst, worst_rgb);
    CHECK(gfx_hsv_to_rgb(0, 255, 255) == 0xff0000 && gfx_hsv_to_rgb(120, 255, 255) == 0x00ff00 &&
              gfx_hsv_to_rgb(240, 255, 255) == 0x0000ff && gfx_hsv_to_rgb(77, 0, 128) == 0x808080 &&
              gfx_hsv_to_rgb(360, 255, 255) == 0xff0000,
          "primary colours and greys");
    int h, s, v;
    gfx_rgb_to_hsv(0x00ffff, &h, &s, &v);
    CHECK(h == 180 && s == 255 && v == 255, "cyan: %d %d %d", h, s, v);

    uint32_t c = 0;
    CHECK(gfx_color_parse("#3060a0", &c) == 0 && c == 0x3060a0 && gfx_color_parse("0x3060A0", &c) == 0 &&
              c == 0x3060a0 && gfx_color_parse("ffffff", &c) == 0 && c == 0xffffff,
          "colour texts");
    CHECK(gfx_color_parse("#3060a", &c) < 0 && gfx_color_parse("#3060a0f", &c) < 0 &&
              gfx_color_parse("#30g0a0", &c) < 0 && gfx_color_parse("", &c) < 0,
          "malformed colour texts");

    int m = display_mode_parse("2560x1600@2");
    char text[32];
    display_mode_format(m, text, sizeof text);
    CHECK(DISPLAY_MODE_W(m) == 2560 && DISPLAY_MODE_H(m) == 1600 && DISPLAY_MODE_S(m) == 2 &&
              strcmp(text, "2560x1600@2") == 0,
          "display mode with a scale: %s", text);
    m = display_mode_parse("1280x800");
    CHECK(m == DISPLAY_MODE_PACK(1280, 800, 1), "display mode without a scale");
    CHECK(display_mode_parse("640x479") == 0 && display_mode_parse("1280x800@5") == 0 &&
              display_mode_parse("1280y800") == 0 && display_mode_parse("1280x800@2x") == 0,
          "display modes out of range");
    const char *const *list;
    int n = display_resolutions(&list);
    CHECK(n == 10 && strcmp(list[0], "1024x768") == 0 && strcmp(list[n - 1], "2560x1600") == 0,
          "resolutions of virtio-gpu: %d", n);
}
