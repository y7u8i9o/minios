/* banner: print words in large letters built from the GUI font. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui/gfx.h>

int main(int argc, char **argv)
{
    char text[64] = "";
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            strncat(text, " ", sizeof text - strlen(text) - 1);
        strncat(text, argv[i], sizeof text - strlen(text) - 1);
    }
    if (!text[0])
        strcpy(text, "minios");
    struct surface s;
    s.width = gfx_text_width(text);
    s.height = GFX_FONT_H;
    s.stride = s.width;
    s.pixels = calloc((size_t)s.width * s.height, sizeof(uint32_t));
    if (!s.pixels)
        return 1;
    gfx_text(&s, 0, 0, text, 1, 0);
    for (int y = 1; y < s.height - 1; y++) {
        for (int x = 0; x < s.width; x++)
            putchar(s.pixels[y * s.stride + x] ? '#' : ' ');
        putchar('\n');
    }
    return 0;
}
