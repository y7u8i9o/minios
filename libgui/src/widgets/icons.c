/* Icon cache: /usr/share/icons/<name>.png decoded once per process. */
#include <gui/widget.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ICON_MAX 64

static struct { char name[32]; struct image *img; } cache[ICON_MAX];
static int ncache;

const struct image *icon_get(const char *name)
{
    for (int i = 0; i < ncache; i++)
        if (strcmp(cache[i].name, name) == 0)
            return cache[i].img;
    if (ncache == ICON_MAX)
        return NULL;
    char path[128];
    snprintf(path, sizeof path, "/usr/share/icons/%s.png", name);
    struct image *img = image_load(path);
    strlcpy(cache[ncache].name, name, sizeof cache[ncache].name);
    cache[ncache].img = img;                /* NULL is cached too */
    ncache++;
    return img;
}
