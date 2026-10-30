/* The resize zones of window frames (gui_resize_edges, gui_resize_region)
 * and their use by the client decorations (csd_hit, csd_input_region). */
#include "check.h"
#include <gui/gfx.h>
#include "../src/csd.h"

void run_resize_tests(void)
{
    /* The zones of the compositor frame: 1 px border, 6 px margin, 24 px
     * corners, squares 12 px outside, 8 px inside at the rounded top. */
    const struct gui_resize_zones z = { .margin = 6, .inner = 1, .corner = 24, .reach = 12, .inset_top = 8, .inset_bottom = 1 };
    struct rect f = { 100, 100, 400, 300 };
    const int TL = GUI_EDGE_TOP | GUI_EDGE_LEFT, TR = GUI_EDGE_TOP | GUI_EDGE_RIGHT;
    const int BL = GUI_EDGE_BOTTOM | GUI_EDGE_LEFT, BR = GUI_EDGE_BOTTOM | GUI_EDGE_RIGHT;
    CHECK(gui_resize_edges(f, &z, 88, 88) == TL, "top left square, 12 px out: %d", gui_resize_edges(f, &z, 88, 88));
    CHECK(gui_resize_edges(f, &z, 107, 107) == TL, "top left square, 7 px in: %d", gui_resize_edges(f, &z, 107, 107));
    CHECK(gui_resize_edges(f, &z, 108, 108) == 0, "8 px inside the top left corner: %d", gui_resize_edges(f, &z, 108, 108));
    CHECK(gui_resize_edges(f, &z, 511, 88) == TR, "top right square: %d", gui_resize_edges(f, &z, 511, 88));
    CHECK(gui_resize_edges(f, &z, 492, 107) == TR, "top right square inside: %d", gui_resize_edges(f, &z, 492, 107));
    CHECK(gui_resize_edges(f, &z, 88, 411) == BL, "bottom left square: %d", gui_resize_edges(f, &z, 88, 411));
    CHECK(gui_resize_edges(f, &z, 101, 398) == 0, "1 px inside the bottom left border: %d", gui_resize_edges(f, &z, 101, 398));
    CHECK(gui_resize_edges(f, &z, 511, 411) == BR, "bottom right square: %d", gui_resize_edges(f, &z, 511, 411));
    CHECK(gui_resize_edges(f, &z, 94, 250) == GUI_EDGE_LEFT, "left margin: %d", gui_resize_edges(f, &z, 94, 250));
    CHECK(gui_resize_edges(f, &z, 93, 250) == 0, "outside the left margin: %d", gui_resize_edges(f, &z, 93, 250));
    CHECK(gui_resize_edges(f, &z, 100, 250) == GUI_EDGE_LEFT, "left border: %d", gui_resize_edges(f, &z, 100, 250));
    CHECK(gui_resize_edges(f, &z, 300, 405) == GUI_EDGE_BOTTOM, "bottom margin: %d", gui_resize_edges(f, &z, 300, 405));
    CHECK(gui_resize_edges(f, &z, 95, 120) == TL, "left margin within 24 px of the corner: %d", gui_resize_edges(f, &z, 95, 120));
    CHECK(gui_resize_edges(f, &z, 95, 124) == GUI_EDGE_LEFT, "left margin 24 px below the corner: %d", gui_resize_edges(f, &z, 95, 124));
    CHECK(gui_resize_edges(f, &z, 300, 250) == 0, "inside the frame: %d", gui_resize_edges(f, &z, 300, 250));
    struct rect region[5];
    CHECK(gui_resize_region(f, &z, region) == 5, "five region rectangles");
    int in_region = 0;
    for (int i = 0; i < 5; i++)
        in_region |= rect_contains(region[i], 88, 88);
    CHECK(in_region, "the region contains the top left square");

    /* The client decorations: the corner square reaches the rounded
     * corner inside, the close button and the contents do not resize. */
    struct csd c = { .enabled = 1, .active = 1 };
    int w = 300, h = 200, edges;
    struct rect cf = csd_frame(&c, w, h);
    CHECK(csd_hit(&c, w, h, cf.x - 12, cf.y - 12, &edges) == CSD_RESIZE && edges == TL, "csd corner outside: %d", edges);
    CHECK(csd_hit(&c, w, h, cf.x + 2, cf.y + 2, &edges) == CSD_RESIZE && edges == TL, "csd rounded corner inside: %d", edges);
    CHECK(csd_hit(&c, w, h, cf.x + cf.w - 9, cf.y + 10, &edges) == CSD_CLOSE, "csd close button");
    CHECK(csd_hit(&c, w, h, cf.x + 40, cf.y + 10, &edges) == CSD_HEADER_BAR, "csd header bar");
    CHECK(csd_hit(&c, w, h, cf.x + 40, cf.y + 100, &edges) == CSD_CONTENT, "csd contents");
    CHECK(csd_hit(&c, w, h, cf.x - 13, cf.y + 100, &edges) == CSD_OUTSIDE, "csd beyond the margin");
    struct rect input[5];
    int n = csd_input_region(&c, w, h, input);
    in_region = 0;
    for (int i = 0; i < n; i++)
        in_region |= rect_contains(input[i], cf.x - 12, cf.y - 12);
    CHECK(n == 5 && in_region, "the csd input region contains the corner square");
    c.maximized = 1;
    CHECK(csd_hit(&c, w, h, 0, 0, &edges) != CSD_RESIZE, "a maximized window has no resize zones");
}
