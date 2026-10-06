/* Display modes in packed and text form (gui/display.h). */
#include <gui/display.h>
#include <stdio.h>
#include <stdlib.h>

int display_mode_parse(const char *text)
{
    char *end;
    long w = strtol(text, &end, 10);
    if (*end != 'x')
        return 0;
    long h = strtol(end + 1, &end, 10);
    long s = 1;
    if (*end == '@')
        s = strtol(end + 1, &end, 10);
    if (*end != '\0' && *end != ' ')
        return 0;
    if (w < 640 || h < 480 || w > 8192 || h > 8192 || s < 1 || s > 4)
        return 0;
    return (int)DISPLAY_MODE_PACK(w, h, s);
}

void display_mode_format(int mode, char *buf, size_t size)
{
    snprintf(buf, size, "%dx%d@%d", DISPLAY_MODE_W(mode), DISPLAY_MODE_H(mode), DISPLAY_MODE_S(mode));
}

int display_resolutions(const char *const **list)
{
    static const char *const resolutions[] = { "1024x768",  "1280x800",  "1280x1024", "1440x900",  "1600x1200",
                                               "1680x1050", "1920x1080", "1920x1200", "2560x1440", "2560x1600" };
    *list = resolutions;
    return (int)(sizeof resolutions / sizeof resolutions[0]);
}
