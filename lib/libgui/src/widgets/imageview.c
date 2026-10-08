/* The image view (gui/widget.h): one image centred, reduced to fit the
 * area with its proportions, never enlarged. */
#include <gui/app.h>
#include <stdint.h>

/* rendition is img scaled for the size rw by rh logical pixels at the
 * scale rscale, made anew when the size or the scale changes. */
struct imageview {
    struct widget w;
    const struct image *img;
    struct image *rendition;
    int rw, rh, rscale;
};

static void imageview_measure(struct widget *w, struct size_hint *h)
{
    const struct image *img = ((struct imageview *)w)->img;
    h->pref_w = img ? image_lw(img) : 0;
    h->pref_h = img ? image_lh(img) : 0;
}

/* The image to draw dw by dh logical pixels at the scale of the painter:
 * the image itself at its own size, else the cached rendition. */
static const struct image *sized(struct imageview *v, int dw, int dh, int scale)
{
    if (dw == image_lw(v->img) && dh == image_lh(v->img) && v->img->scale == scale)
        return v->img;
    if (v->rendition && v->rw == dw && v->rh == dh && v->rscale == scale)
        return v->rendition;
    image_free(v->rendition);
    v->rendition = image_scale(v->img, dw * scale, dh * scale);
    if (v->rendition)
        v->rendition->scale = scale;
    v->rw = dw;
    v->rh = dh;
    v->rscale = scale;
    return v->rendition;
}

static void imageview_paint(struct widget *w, struct painter *p)
{
    struct imageview *v = (struct imageview *)w;
    painter_fill(p, 0, 0, w->w, w->h, p->theme->color[TC_WINDOW]);
    if (v->img && w->w > 0 && w->h > 0) {
        int iw = image_lw(v->img), ih = image_lh(v->img), dw = iw, dh = ih;
        if (dw > w->w) {
            dw = w->w;
            dh = (int)((int64_t)ih * dw / iw);
        }
        if (dh > w->h) {
            dh = w->h;
            dw = (int)((int64_t)iw * dh / ih);
        }
        const struct image *s = dw > 0 && dh > 0 ? sized(v, dw, dh, p->scale) : NULL;
        if (s)
            painter_image(p, (w->w - dw) / 2, (w->h - dh) / 2, s);
    }
    struct sig_paint s = { p };
    widget_emit(w, "paint", &s);
}

static void imageview_destroy(struct widget *w)
{
    image_free(((struct imageview *)w)->rendition);
}

const struct widget_class imageview_class = { "imageview", sizeof(struct imageview), imageview_measure, NULL,
                                              imageview_paint, NULL, imageview_destroy };

struct widget *imageview_new(struct widget *parent)
{
    return widget_new(&imageview_class, parent);
}

void imageview_set(struct widget *w, const struct image *img)
{
    struct imageview *v = (struct imageview *)w;
    if (v->img == img)
        return;
    v->img = img;
    image_free(v->rendition);
    v->rendition = NULL;
    widget_relayout(w);
}
