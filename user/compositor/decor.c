/* Server side decorations of toplevels: title bar with boxes, border,
 * resize grip, and the move and resize drags (also started by a
 * client's move and resize requests). */
#include <stdio.h>
#include <string.h>
#include "comp.h"

#define COLOR_TITLE          0x00204060
#define COLOR_TITLE_INACTIVE 0x00707070
#define COLOR_TITLE_FG       0x00ffffff
#define COLOR_BORDER         0x00101010
#define COLOR_CLOSE          0x00c04040
#define COLOR_BUTTON         0x00507090

static struct toplevel *drag;
static int drag_dx, drag_dy;
static struct toplevel *resizing;
static int resize_edges, resize_x0, resize_y0, resize_w0, resize_h0, resize_sx, resize_sy;

int decor_has(const struct csurface *s)
{
    return s->role == ROLE_TOPLEVEL && s->toplevel && s->toplevel->decor_mode == DECOR_SERVER;
}

struct rect decor_frame(const struct csurface *s)
{
    struct rect r = { s->x - BORDER, s->y - TITLE_H - BORDER, s->width + 2 * BORDER, s->height + TITLE_H + 2 * BORDER };
    return r;
}

static struct rect button_rect(const struct csurface *s, int n)
{
    struct rect r = { s->x + s->width - 16 - n * (TITLE_BTN + 2), s->y - TITLE_H + 2, TITLE_BTN, TITLE_BTN };
    return r;
}

static struct rect grip_rect(const struct csurface *s)
{
    struct rect r = { s->x + s->width - GRIP, s->y + s->height - GRIP, GRIP, GRIP };
    return r;
}

void decor_draw(struct csurface *s, struct rect clip)
{
    struct rect frame = decor_frame(s);
    struct rect c = rect_intersect(frame, clip);
    if (rect_empty(c))
        return;
    struct surface v = { back.pixels + (size_t)c.y * back.stride + c.x, c.w, c.h, back.stride };
    int ox = -c.x, oy = -c.y;
    int active = s->toplevel->activated;
    gfx_fill_rect(&v, frame.x + ox, frame.y + oy, frame.w, TITLE_H + BORDER, active ? COLOR_TITLE : COLOR_TITLE_INACTIVE);
    gfx_rect(&v, frame.x + ox, frame.y + oy, frame.w, frame.h, COLOR_BORDER);
    gfx_text(&v, s->x + 4 + ox, s->y - TITLE_H + 2 + oy, s->toplevel->title, COLOR_TITLE_FG, 0xffffffffu);
    for (int n = 0; n < 3; n++) {
        struct rect b = button_rect(s, n);
        int bx = b.x + ox, by = b.y + oy;
        gfx_fill_rect(&v, bx, by, b.w, b.h, n == 0 ? COLOR_CLOSE : COLOR_BUTTON);
        if (n == 0) {
            gfx_line(&v, bx + 3, by + 3, bx + 10, by + 10, COLOR_TITLE_FG);
            gfx_line(&v, bx + 10, by + 3, bx + 3, by + 10, COLOR_TITLE_FG);
        } else if (n == 1) {
            gfx_rect(&v, bx + 3, by + 3, 8, 8, COLOR_TITLE_FG);
            gfx_hline(&v, bx + 3, by + 4, 8, COLOR_TITLE_FG);
        } else {
            gfx_fill_rect(&v, bx + 3, by + 9, 8, 2, COLOR_TITLE_FG);
        }
    }
    if (!s->toplevel->maximized) {
        struct rect g = grip_rect(s);
        for (int i = 2; i < GRIP; i += 4)
            gfx_line(&v, g.x + i + ox, g.y + GRIP - 1 + oy, g.x + GRIP - 1 + ox, g.y + i + oy, COLOR_BORDER);
    }
}

/* button: 1 = left with the pointer on the decorations; bit 0x100 marks
 * a move requested by the client, 0x200 a resize with the edges in
 * bits 16 and up. */
int decor_press(struct csurface *s, int button)
{
    struct toplevel *t = s->toplevel;
    if (!t)
        return 0;
    if (button & 0x100) {
        drag = t;
        drag_dx = cursor_x - s->x;
        drag_dy = cursor_y - s->y;
        return 1;
    }
    if (button & 0x200) {
        resizing = t;
        resize_edges = button >> 16;
        resize_x0 = cursor_x;
        resize_y0 = cursor_y;
        resize_w0 = s->width;
        resize_h0 = s->height;
        resize_sx = s->x;
        resize_sy = s->y;
        return 1;
    }
    if (!decor_has(s) || !(button & 1))
        return 0;
    if (seat_modifiers() & 4) {                 /* Alt drag from anywhere */
        if (!t->maximized) {
            drag = t;
            drag_dx = cursor_x - s->x;
            drag_dy = cursor_y - s->y;
        }
        return 1;
    }
    if (cursor_y < s->y) {
        if (rect_contains(button_rect(s, 0), cursor_x, cursor_y))
            toplevel_close(t);
        else if (rect_contains(button_rect(s, 1), cursor_x, cursor_y))
            toplevel_set_maximized(t, !t->maximized);
        else if (rect_contains(button_rect(s, 2), cursor_x, cursor_y))
            toplevel_set_minimized(t, 1);
        else if (!t->maximized) {
            drag = t;
            drag_dx = cursor_x - s->x;
            drag_dy = cursor_y - s->y;
        }
        return 1;
    }
    if (!t->maximized && rect_contains(grip_rect(s), cursor_x, cursor_y)) {
        resizing = t;
        resize_edges = EDGE_BOTTOM | EDGE_RIGHT;
        resize_x0 = cursor_x;
        resize_y0 = cursor_y;
        resize_w0 = s->width;
        resize_h0 = s->height;
        resize_sx = s->x;
        resize_sy = s->y;
        return 1;
    }
    return 0;
}

int decor_dragging(void)
{
    return drag != NULL || resizing != NULL;
}

int decor_motion(void)
{
    if (drag) {
        toplevel_move(drag, cursor_x - drag_dx, cursor_y - drag_dy);
        return 1;
    }
    return resizing != NULL;
}

int decor_release(void)
{
    if (drag) {
        drag = NULL;
        return 1;
    }
    if (resizing) {
        struct toplevel *t = resizing;
        resizing = NULL;
        int dx = cursor_x - resize_x0, dy = cursor_y - resize_y0;
        int w = resize_w0, h = resize_h0;
        if (resize_edges & EDGE_RIGHT) w += dx;
        if (resize_edges & EDGE_LEFT) { w -= dx; t->s->x = resize_sx + dx; }
        if (resize_edges & EDGE_BOTTOM) h += dy;
        if (resize_edges & EDGE_TOP) { h -= dy; t->s->y = resize_sy + dy; }
        if (w < 32) w = 32;
        if (h < 32) h = 32;
        toplevel_configure(t, w, h);
        return 1;
    }
    return 0;
}

/* Whether the point lies on the decorations of s (not its contents). */
int decor_hit(const struct csurface *s, int x, int y);
int decor_hit(const struct csurface *s, int x, int y)
{
    if (!decor_has(s))
        return 0;
    if (!s->toplevel->maximized && rect_contains(grip_rect(s), x, y))
        return 1;
    return rect_contains(decor_frame(s), x, y) && !rect_contains(surface_rect(s), x, y);
}
