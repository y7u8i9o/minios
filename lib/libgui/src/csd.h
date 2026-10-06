#pragma once
/* Client side decorations, drawn by the toolkit into an ARGB buffer
 * around the window contents the way GTK 4 does: a flat header bar with
 * the title and the window buttons, a hairline outline, small rounded
 * corners and a light drop shadow. The compositor only learns the window
 * geometry. */
#include <gui/gfx.h>

#define CSD_MARGIN 16          /* shadow reach around the frame */
#define CSD_HEADER 30          /* header bar height, bottom hairline included */
#define CSD_RADIUS 6
#define CSD_BORDER_ZONE 8      /* resize zone outside the frame, inside the margin */
#define CSD_BUTTON 22
#define CSD_CORNER 24          /* corner resize zones along the edges */
#define CSD_CORNER_REACH 12    /* corner squares outside the frame, inside the margin */
/* The chrome around the frame: a drop shadow CSD_SHADOW_REACH logical
 * pixels wide and CSD_SHADOW_DY lower than the frame, with the peak alpha
 * of an active window and of the others out of 255, and an outline of
 * CSD_OUTLINE_ALPHA. */
#define CSD_SHADOW_REACH 8
#define CSD_SHADOW_DY 2
#define CSD_SHADOW_PEAK 64
#define CSD_SHADOW_PEAK_BACKDROP 32
#define CSD_OUTLINE_ALPHA 51   /* 20 percent black */

enum csd_zone { CSD_OUTSIDE, CSD_CONTENT, CSD_HEADER_BAR, CSD_CLOSE, CSD_MAXIMIZE, CSD_MINIMIZE, CSD_RESIZE };

struct csd {
    int enabled;               /* the compositor granted client decorations */
    int maximized, active;
    int hover;                 /* CSD_CLOSE, CSD_MAXIMIZE, CSD_MINIMIZE or 0 */
    char title[64];
};

int csd_margin(const struct csd *c);
int csd_header(const struct csd *c);
/* Buffer size for contents of w by h logical pixels; the frame (the
 * window geometry) and the contents inside the buffer. */
void csd_buffer_size(const struct csd *c, int w, int h, int *bw, int *bh);
struct rect csd_frame(const struct csd *c, int w, int h);
struct rect csd_content(const struct csd *c, int w, int h);
/* What lies at (x, y) of the buffer; edges (GUI_EDGE_*) for CSD_RESIZE. */
enum csd_zone csd_hit(const struct csd *c, int w, int h, int x, int y, int *edges);
/* Regions in surface coordinates; the number of rectangles. */
int csd_opaque_region(const struct csd *c, int w, int h, struct rect out[4]);
int csd_input_region(const struct csd *c, int w, int h, struct rect out[5]);
/* Paint the chrome (everything but the contents) into buf, scale device
 * pixels per logical pixel; returns the painted device rectangle. */
struct rect csd_paint(struct surface *buf, int scale, const struct csd *c, int w, int h);
/* Repaint the header bar only (title, hover). */
struct rect csd_paint_header(struct surface *buf, int scale, const struct csd *c, int w, int h);
/* Copy the device rectangle r of src (the drawing surface: 0x00RRGGBB
 * inside the frame, ARGB chrome around it) into dst, the buffer sent to
 * the compositor: the frame becomes opaque, its rounded corners blend
 * the drawing over the chrome. */
void csd_copy(struct surface *dst, const struct surface *src, struct rect r, int scale, const struct csd *c, int w, int h);
