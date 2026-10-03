/* This file contains the images of the gui module. It loads PNG and SVG
 * files and pixel strings into struct image (gui/image.h), defines the
 * image view widget, and makes the resampled renditions that the
 * painter and the view draw at other sizes.
 *
 * An image is a full userdata that owns its struct image and one cached
 * rendition at another size. Only the garbage collector frees it. A
 * label, button or image view showing an image retains the userdata in
 * the widget's handler table, so the pixels outlive every widget that
 * points at them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <gui/app.h>
#include "lauxlib.h"
#include "minios.h"
#include "lgui.h"

/* An image built or resampled here has sides of at most this many
 * device pixels, which retains width * height * 4 far inside size_t and
 * int. */
#define IMAGE_SIDE_MAX 16384
/* image_render_svg refuses larger renderings. */
#define SVG_PX_MAX 1024

struct limage {
    struct image *img;          /* owned */
    struct image *sized;        /* the last rendition at another size, owned; NULL when none */
    int sized_w, sized_h;       /* its size in logical pixels */
};

static struct image *image_alloc(int w, int h, int scale)
{
    struct image *img = malloc(sizeof *img);
    uint32_t *pixels = calloc((size_t)w * (size_t)h, 4);
    if (!img || !pixels) {
        free(img);
        free(pixels);
        return NULL;
    }
    img->w = w;
    img->h = h;
    img->pixels = pixels;
    img->scale = scale;
    return img;
}

static void push_image(lua_State *L, struct image *img)
{
    struct limage *li = lua_newuserdatauv(L, sizeof *li, 0);
    li->img = img;
    li->sized = NULL;
    li->sized_w = li->sized_h = 0;
    luaL_setmetatable(L, GUI_IMAGE_META);
}

static struct limage *check_limage(lua_State *L, int index)
{
    return luaL_checkudata(L, index, GUI_IMAGE_META);
}

const struct image *gui_check_image(lua_State *L, int index)
{
    return check_limage(L, index)->img;
}

/* A box filter over premultiplied samples resamples the image. Each
 * target pixel averages the source pixels its area covers, which is the
 * nearest pixel when the image is enlarged. */
static struct image *resample(const struct image *src, int dw, int dh, int scale)
{
    struct image *d = image_alloc(dw, dh, scale);
    if (!d)
        return NULL;
    for (int j = 0; j < dh; j++) {
        int y0 = (int)((int64_t)j * src->h / dh), y1 = (int)((int64_t)(j + 1) * src->h / dh);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int i = 0; i < dw; i++) {
            int x0 = (int)((int64_t)i * src->w / dw), x1 = (int)((int64_t)(i + 1) * src->w / dw);
            if (x1 <= x0)
                x1 = x0 + 1;
            uint64_t sa = 0, sr = 0, sg = 0, sb = 0;
            for (int y = y0; y < y1; y++) {
                const uint32_t *row = src->pixels + (size_t)y * src->w;
                for (int x = x0; x < x1; x++) {
                    uint32_t c = row[x], a = c >> 24;
                    sa += a;
                    sr += ((c >> 16) & 255) * a;
                    sg += ((c >> 8) & 255) * a;
                    sb += (c & 255) * a;
                }
            }
            uint64_t n = (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
            if (!sa)
                continue;               /* calloc left the pixel transparent */
            uint32_t a = (uint32_t)((sa + n / 2) / n);
            uint32_t r = (uint32_t)((sr + sa / 2) / sa), g = (uint32_t)((sg + sa / 2) / sa),
                     b = (uint32_t)((sb + sa / 2) / sa);
            d->pixels[(size_t)j * dw + i] = a << 24 | r << 16 | g << 8 | b;
        }
    }
    return d;
}

/* Returns the image to draw w by h logical pixels at a painter scale.
 * That is the image itself at its own size, else the cached rendition,
 * which is made anew when the size or the scale changed. The result is
 * NULL when memory runs out. */
static const struct image *limage_sized(struct limage *li, int w, int h, int scale)
{
    if (w == image_lw(li->img) && h == image_lh(li->img))
        return li->img;
    if (li->sized && li->sized_w == w && li->sized_h == h && li->sized->scale == scale)
        return li->sized;
    image_free(li->sized);
    li->sized = resample(li->img, w * scale, h * scale, scale);
    li->sized_w = w;
    li->sized_h = h;
    return li->sized;
}

const struct image *gui_image_sized(lua_State *L, int index, int w, int h, int scale)
{
    struct limage *li = check_limage(L, index);
    if (scale < 1)
        scale = 1;
    if (w > IMAGE_SIDE_MAX / scale || h > IMAGE_SIDE_MAX / scale)
        luaL_error(L, "image size %dx%d too large", w, h);
    return limage_sized(li, w, h, scale);
}

/* ---- loading ---- */

/* Returns the scale of the first output, which the icon cache uses as
 * well, or 1 without a connection. */
static int output_scale(void)
{
    struct gui_output_info info;
    if (gui_output_count() > 0 && gui_get_output(0, &info) == 0 && info.scale > 0 && info.scale <= 4)
        return info.scale;
    return 1;
}

static int is_svg(const char *path)
{
    size_t n = strlen(path);
    if (n < 4)
        return 0;
    const char *e = path + n - 4;
    return e[0] == '.' && (e[1] | 0x20) == 's' && (e[2] | 0x20) == 'v' && (e[3] | 0x20) == 'g';
}

/* gui.image(path [, size [, color]]) loads a PNG file at its own size,
 * or an SVG file (by the .svg suffix) rendered size by size logical
 * pixels (16 by default) at the output's scale, with color filling the
 * paths that name no fill. It returns nil, the message and the errno on
 * failure. */
static int g_image(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    struct image *img;
    int svg = is_svg(path);
    errno = 0;
    if (svg) {
        lua_Integer size = luaL_optinteger(L, 2, 16);
        uint32_t color = (uint32_t)luaL_optinteger(L, 3, 0) & 0x00ffffff;
        int scale = output_scale();
        luaL_argcheck(L, size >= 1 && size <= SVG_PX_MAX / scale, 2, "size out of range");
        img = image_load_svg(path, (int)size * scale, color);
        if (img)
            img->scale = scale;
    } else {
        img = image_load(path);
    }
    if (!img) {
        int e = errno ? errno : EINVAL;
        lua_pushnil(L);
        if (e == EINVAL)
            lua_pushfstring(L, "%s: not a valid %s file", path, svg ? "SVG" : "PNG");
        else
            lua_pushfstring(L, "%s: %s", path, strerror(e));
        lua_pushinteger(L, e);
        return 3;
    }
    push_image(L, img);
    return 1;
}

/* gui.from_pixels(w, h [, data]) builds an image of w by h pixels from
 * a string of w * h four byte pixels, each 0xAARRGGBB in little endian
 * order (string.pack("<I4", argb)), row by row. The image is
 * transparent without data. */
static int g_from_pixels(lua_State *L)
{
    lua_Integer w = luaL_checkinteger(L, 1), h = luaL_checkinteger(L, 2);
    luaL_argcheck(L, w >= 1 && w <= IMAGE_SIDE_MAX, 1, "width out of range");
    luaL_argcheck(L, h >= 1 && h <= IMAGE_SIDE_MAX, 2, "height out of range");
    size_t len = 0;
    const char *data = luaL_optlstring(L, 3, NULL, &len);
    size_t need = (size_t)w * (size_t)h * 4;
    if (data && len != need)
        return luaL_argerror(L, 3, lua_pushfstring(L, "%d bytes expected, got %d", (int)need, (int)len));
    struct image *img = image_alloc((int)w, (int)h, 1);
    if (!img)
        return luaL_error(L, "not enough memory");
    if (data)
        memcpy(img->pixels, data, need);
    push_image(L, img);
    return 1;
}

/* ---- image methods ---- */

/* image:size() returns the width and height in logical pixels. */
static int i_size(lua_State *L)
{
    const struct image *img = gui_check_image(L, 1);
    lua_pushinteger(L, image_lw(img));
    lua_pushinteger(L, image_lh(img));
    return 2;
}

/* image:scale() returns the device pixels per logical pixel. */
static int i_scale(lua_State *L)
{
    const struct image *img = gui_check_image(L, 1);
    lua_pushinteger(L, img->scale > 1 ? img->scale : 1);
    return 1;
}

/* image:pixel(x, y [, argb]) returns the pixel at device coordinates
 * from 0, or stores argb there and returns the image. */
static int i_pixel(lua_State *L)
{
    struct limage *li = check_limage(L, 1);
    lua_Integer x = luaL_checkinteger(L, 2), y = luaL_checkinteger(L, 3);
    if (x < 0 || y < 0 || x >= li->img->w || y >= li->img->h)
        return luaL_error(L, "pixel %d,%d outside the image", (int)x, (int)y);
    uint32_t *at = &li->img->pixels[(size_t)y * li->img->w + (size_t)x];
    if (lua_isnoneornil(L, 4)) {
        lua_pushinteger(L, *at);
        return 1;
    }
    *at = (uint32_t)luaL_checkinteger(L, 4);
    image_free(li->sized);              /* the rendition no longer matches */
    li->sized = NULL;
    lua_settop(L, 1);
    return 1;
}

/* image:pixels() returns every device pixel in the format of
 * gui.from_pixels. */
static int i_pixels(lua_State *L)
{
    const struct image *img = gui_check_image(L, 1);
    lua_pushlstring(L, (const char *)img->pixels, (size_t)img->w * (size_t)img->h * 4);
    return 1;
}

static int i_gc(lua_State *L)
{
    struct limage *li = check_limage(L, 1);
    image_free(li->sized);
    image_free(li->img);
    li->sized = li->img = NULL;
    return 0;
}

static int i_tostring(lua_State *L)
{
    struct limage *li = check_limage(L, 1);
    if (li->img)
        lua_pushfstring(L, "image: %dx%d", image_lw(li->img), image_lh(li->img));
    else
        lua_pushstring(L, "image: freed");
    return 1;
}

static const luaL_Reg image_methods[] = {
    { "size", i_size }, { "scale", i_scale }, { "pixel", i_pixel }, { "pixels", i_pixels },
    { "__gc", i_gc }, { "__tostring", i_tostring },
    { NULL, NULL }
};

/* ---- the image view ---- */

/* The image view shows one image centred, reduced to fit its area with
 * the proportions retained and never enlarged. After the image it emits
 * "paint", so a handler can draw over it. */
struct imageview {
    struct widget base;
    struct limage *image;       /* the userdata in the handler table; NULL when none */
};

static void imageview_measure(struct widget *w, struct size_hint *h)
{
    struct limage *li = ((struct imageview *)w)->image;
    h->pref_w = li ? image_lw(li->img) : 0;
    h->pref_h = li ? image_lh(li->img) : 0;
}

static void imageview_paint(struct widget *w, struct painter *p)
{
    struct limage *li = ((struct imageview *)w)->image;
    painter_fill(p, 0, 0, w->w, w->h, p->theme->color[TC_WINDOW]);
    if (li && w->w > 0 && w->h > 0) {
        int iw = image_lw(li->img), ih = image_lh(li->img), dw = iw, dh = ih;
        if (dw > w->w) {
            dw = w->w;
            dh = (int)((int64_t)ih * dw / iw);
        }
        if (dh > w->h) {
            dh = w->h;
            dw = (int)((int64_t)iw * dh / ih);
        }
        if (dw > 0 && dh > 0) {
            const struct image *s = limage_sized(li, dw, dh, p->scale);
            if (s)
                painter_image(p, (w->w - dw) / 2, (w->h - dh) / 2, s);
        }
    }
    struct sig_paint s = { p };
    widget_emit(w, "paint", &s);
}

static const struct widget_class imageview_class = {
    "imageview", sizeof(struct imageview), imageview_measure, NULL, imageview_paint, NULL, NULL
};

/* Retains the image userdata at index (or nil) in the handler table of the
 * widget at index 1, under "image". */
static void retain_image(lua_State *L, int index)
{
    lua_getiuservalue(L, 1, 1);
    lua_pushvalue(L, index);
    lua_setfield(L, -2, "image");
    lua_pop(L, 1);
}

/* widget:image(img | nil) sets the image of an image view, or the icon
 * of a label or button, which draw it beside their caption. */
int gui_widget_image(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    struct limage *li = lua_isnoneornil(L, 2) ? NULL : check_limage(L, 2);
    lua_settop(L, 2);
    if (w->cls == &imageview_class) {
        ((struct imageview *)w)->image = li;
        widget_relayout(w);
    } else if (strcmp(w->cls->name, "label") == 0 || strcmp(w->cls->name, "button") == 0) {
        widget_set_icon(w, li ? li->img : NULL);
    } else {
        return luaL_error(L, "image: %s shows no image", w->cls->name);
    }
    retain_image(L, 2);
    lua_settop(L, 1);
    return 1;
}

/* gui.imageview(parent [, img]) creates an image view. */
static int g_imageview(lua_State *L)
{
    struct widget *parent = gui_check_widget(L, 1);
    struct limage *li = lua_isnoneornil(L, 2) ? NULL : check_limage(L, 2);
    struct widget *w = widget_new(&imageview_class, parent);
    if (!w)
        return luaL_error(L, "not enough memory");
    ((struct imageview *)w)->image = li;
    gui_push_widget(L, w);
    lua_replace(L, 1);
    lua_settop(L, 2);
    retain_image(L, 2);
    lua_settop(L, 1);
    return 1;
}

static const luaL_Reg image_funcs[] = {
    { "image", g_image }, { "from_pixels", g_from_pixels }, { "imageview", g_imageview },
    { NULL, NULL }
};

void gui_open_image(lua_State *L)
{
    luaL_newmetatable(L, GUI_IMAGE_META);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, image_methods, 0);
    lua_pop(L, 1);
    luaL_setfuncs(L, image_funcs, 0);
}
