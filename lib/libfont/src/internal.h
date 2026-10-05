#pragma once
/* Internals shared by the libfont source files. */
#include <font/font.h>
#include <string.h>

/* Point kinds: on_curve 1 is a point on the outline, 0 a quadratic
 * control point, 2 a cubic control point (always in pairs). */
#define PT_ON 1
#define PT_QUAD 0
#define PT_CUBIC 2

struct cff_info {
    int present;
    uint32_t base;              /* offset of the CFF table */
    uint32_t charstrings;       /* offset of the CharStrings INDEX */
    int ncharstrings;
    uint32_t gsubrs;            /* offset of the Global Subr INDEX, 0 when none */
    uint32_t subrs;             /* Local Subr INDEX of the Private DICT, 0 when none */
    int is_cid;
    uint32_t fdarray, fdselect; /* CID fonts: per glyph Private DICTs */
};

struct cache_entry {
    int glyph, px;
    uint32_t stamp;
    struct font_glyph g;
};

struct ofont {
    char *path;
    uint8_t *data;
    size_t size;
    int mapped;                 /* data is a read only mapping of the file, not a heap copy */
    int upem, nglyphs;
    int ascent, descent, line_gap;
    int long_loca;
    int num_hmetrics;
    uint32_t hmtx, hmtx_len, loca, loca_len, glyf, glyf_len;
    uint32_t cmap_sub;          /* offset of the chosen cmap subtable */
    int cmap_format;
    uint32_t kern, kern_len, gpos, gpos_len;
    struct cff_info cff;
    struct cache_entry cache[FONT_CACHE_SIZE];
    uint32_t stamp;
};

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline int16_t rds16(const uint8_t *p) { return (int16_t)rd16(p); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint32_t rd24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }

/* Growable outline used by both parsers. */
int outline_add_point(struct font_outline *o, int32_t x, int32_t y, int kind);
int outline_close_contour(struct font_outline *o);
void outline_bounds(struct font_outline *o);

/* ttf.c */
int glyf_outline(const struct ofont *f, int glyph, struct font_outline *o, int depth);
/* cff.c */
int cff_parse(struct ofont *f, uint32_t off, uint32_t len);
int cff_outline(const struct ofont *f, int glyph, struct font_outline *o);
/* raster.c */
void cache_free(struct ofont *f);
/* kern.c */
int gpos_kern(const struct ofont *f, int left, int right, int *found);
int kern_table(const struct ofont *f, int left, int right, int *found);
