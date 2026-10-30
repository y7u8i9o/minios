#!/usr/bin/env python3
"""Generate the cursor set of user/share/cursors (docs/design/cursors.md).

Usage: tools/gencursors.py [DIR]

The shapes below are designed on a grid of 24 by 24 units, and the
script scales them by SCALE into a view box of SIZE by SIZE units. X12
renders the view box at SIZE logical pixels times the screen scale, one
unit per logical pixel. The outline is OUTLINE units wide after the
scaling, so the outline remains one pixel wide. A cursor consists
of groups. Each group is drawn twice: first as a white halo, the shapes
filled and stroked in white OUTLINE units wider on every side, then as the
black body. A later group therefore lies over an earlier one with its own
halo. A badge, such as the plus of copy, remains readable over the
arrow. A glass part is a white area of the body without a halo, such as
the lens of the zoom cursors. Detail strokes are white lines over the
body, such as the gaps between the fingers of a hand.

The script writes NAME.svg for every cursor and the file index with one
line per cursor, "NAME X Y" with the hotspot in view box units, and one
line per alias, "NAME = TARGET". The names are those of the CSS cursor
property.
"""
import math
import os
import sys

SIZE = 20           # the view box and the size of a cursor in logical pixels
SCALE = SIZE / 24.0 # from the design grid to the view box
OUTLINE = 1.0       # the width of the white halo on each side, in view box units


# ---- geometry ----

def rot(points, degrees, cx=12.0, cy=12.0):
    a = math.radians(degrees)
    c, s = math.cos(a), math.sin(a)
    return [(cx + (x - cx) * c - (y - cy) * s, cy + (x - cx) * s + (y - cy) * c) for x, y in points]


def rrect(x, y, w, h, r):
    """A rounded rectangle as a polygon, six segments per corner."""
    r = min(r, w / 2, h / 2)
    pts = []
    for cx, cy, start in ((x + w - r, y + r, -90), (x + w - r, y + h - r, 0), (x + r, y + h - r, 90),
                          (x + r, y + r, 180)):
        for i in range(7):
            a = math.radians(start + 15 * i)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def circle(cx, cy, r, n=32):
    return [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]


def arc(cx, cy, r, a0, a1, n=24):
    """An open polyline along a circle from angle a0 to a1 in degrees."""
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
             cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]


def fmt(v):
    return ("%.2f" % v).rstrip("0").rstrip(".")


def path(points, closed=True):
    d = "M" + " L".join("%s %s" % (fmt(x * SCALE), fmt(y * SCALE)) for x, y in points)
    return d + (" Z" if closed else "")


# A part is ("fill", points), ("line", points, width), ("glass", points)
# or ("detail", points, width). A group is a list of parts.

def fill(points):
    return ("fill", points)


def line(points, width):
    return ("line", points, width)


def detail(points, width):
    return ("detail", points, width)


def glass(points):
    return ("glass", points)


def svg(groups):
    out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d">' % (SIZE, SIZE)]
    for group in groups:
        for part in group:
            if part[0] == "fill":
                out.append('<path fill="#fff" stroke="#fff" stroke-width="%s" stroke-linejoin="round" d="%s"/>'
                           % (fmt(2 * OUTLINE), path(part[1])))
            elif part[0] == "line":
                out.append('<path fill="none" stroke="#fff" stroke-width="%s" stroke-linecap="round" '
                           'stroke-linejoin="round" d="%s"/>' % (fmt(part[2] * SCALE + 2 * OUTLINE),
                                                                 path(part[1], False)))
        for part in group:
            if part[0] == "glass":
                out.append('<path fill="#fff" d="%s"/>' % path(part[1]))
            elif part[0] == "fill":
                out.append('<path fill="#000" d="%s"/>' % path(part[1]))
            elif part[0] == "line":
                out.append('<path fill="none" stroke="#000" stroke-width="%s" stroke-linecap="round" '
                           'stroke-linejoin="round" d="%s"/>' % (fmt(part[2] * SCALE), path(part[1], False)))
        for part in group:
            if part[0] == "detail":
                out.append('<path fill="none" stroke="#fff" stroke-width="%s" stroke-linecap="round" d="%s"/>'
                           % (fmt(part[2] * SCALE), path(part[1], False)))
    out.append("</svg>")
    return "\n".join(out) + "\n"


# ---- shapes ----

# The left edge and the tip lie on whole units of the view box (4.8 and
# 2.4 times SCALE), so that the white outline covers whole pixels at
# scale 1.
ARROW = [(4.8, 2.4), (4.8, 19.7), (9.1, 15.9), (11.7, 21.8), (14.4, 20.6), (11.9, 14.9), (17.4, 14.9)]
ARROW_HOT = (4.8, 2.4)


def arrow():
    return [fill(ARROW)]


def double_arrow(length=19.0, head=5.0, half_head=4.5, half_shaft=1.4):
    """A horizontal arrow with heads at both ends, centred in the box."""
    a, b = 12 - length / 2, 12 + length / 2
    return [(a, 12), (a + head, 12 - half_head), (a + head, 12 - half_shaft), (b - head, 12 - half_shaft),
            (b - head, 12 - half_head), (b, 12), (b - head, 12 + half_head), (b - head, 12 + half_shaft),
            (a + head, 12 + half_shaft), (a + head, 12 + half_head)]


def split_arrows():
    """Two arrows pointing away from a bar in the middle (col-resize)."""
    left = [(2.5, 12), (7.0, 7.8), (7.0, 10.6), (9.6, 10.6), (9.6, 13.4), (7.0, 13.4), (7.0, 16.2)]
    right = [(24 - x, y) for x, y in left]
    bar = rrect(10.9, 3.5, 2.2, 17, 0.6)
    return [fill(left), fill(right), fill(bar)]


def head(tip_x, tip_y, degrees, size=4.2, depth=4.0):
    """A triangular arrow head with its tip at (tip_x, tip_y), pointing in
    the direction degrees (0 to the right, 90 down)."""
    pts = [(tip_x, tip_y), (tip_x - depth, tip_y - size), (tip_x - depth, tip_y + size)]
    return rot(pts, degrees, tip_x, tip_y)


def hand_open():
    fingers = [rrect(7.0, 4.5, 2.8, 10, 1.4), rrect(9.9, 3.0, 2.8, 11, 1.4), rrect(12.8, 3.6, 2.8, 10, 1.4),
               rrect(15.7, 5.6, 2.6, 8.5, 1.3)]
    palm = rrect(7.0, 10.5, 11.3, 10.5, 3.6)
    thumb = line([(8.6, 17.6), (4.4, 12.6)], 2.8)
    gaps = [detail([(9.85, 7.5), (9.85, 12.2)], 0.7), detail([(12.75, 6.5), (12.75, 12.2)], 0.7),
            detail([(15.65, 7.5), (15.65, 12.2)], 0.7)]
    return [fill(f) for f in fingers] + [fill(palm), thumb] + gaps


def hand_fist():
    knuckles = [rrect(7.2, 8.0, 2.9, 5.5, 1.4), rrect(10.1, 7.2, 2.9, 6.0, 1.4), rrect(13.0, 7.5, 2.9, 5.8, 1.4),
                rrect(15.9, 8.5, 2.6, 5.0, 1.3)]
    palm = rrect(6.6, 10.5, 12.0, 10.0, 3.6)
    thumb = line([(8.2, 17.4), (5.2, 13.8)], 2.8)
    gaps = [detail([(10.05, 9.6), (10.05, 12.4)], 0.7), detail([(12.95, 9.2), (12.95, 12.4)], 0.7),
            detail([(15.85, 9.8), (15.85, 12.4)], 0.7)]
    return [fill(k) for k in knuckles] + [fill(palm), thumb] + gaps


def hand_point():
    index = rrect(8.2, 2.0, 3.1, 11.5, 1.55)
    others = [rrect(11.3, 8.2, 2.9, 5.5, 1.4), rrect(14.2, 8.9, 2.8, 5.0, 1.4), rrect(17.0, 9.9, 2.5, 4.3, 1.25)]
    palm = rrect(8.2, 10.8, 11.3, 10.2, 3.6)
    thumb = line([(9.8, 17.6), (5.8, 13.2)], 2.8)
    gaps = [detail([(11.25, 10.4), (11.25, 12.6)], 0.7), detail([(14.15, 10.8), (14.15, 12.6)], 0.7),
            detail([(16.95, 11.6), (16.95, 12.6)], 0.7)]
    return [fill(index)] + [fill(o) for o in others] + [fill(palm), thumb] + gaps


def ibeam():
    return [line([(12, 5.5), (12, 18.5)], 1.6),
            line([(9.2, 4.2), (10.6, 4.4), (11.5, 4.9), (12, 5.6), (12.5, 4.9), (13.4, 4.4), (14.8, 4.2)], 1.4),
            line([(9.2, 19.8), (10.6, 19.6), (11.5, 19.1), (12, 18.4), (12.5, 19.1), (13.4, 19.6), (14.8, 19.8)], 1.4)]


def badge_spinner(cx=18.0, cy=18.0, r=3.4):
    return [line(arc(cx, cy, r, -90, 180), 1.8)]


def badge_question():
    return [line([(15.8, 14.6), (15.9, 13.6), (16.6, 12.7), (17.7, 12.3), (18.8, 12.5), (19.7, 13.3), (20.0, 14.4),
                  (19.6, 15.5), (18.6, 16.3), (18.0, 17.2), (18.0, 18.2)], 1.7),
            fill(circle(18.0, 21.0, 1.15, 16))]


def badge_menu():
    return [fill(rrect(13.8, 13.2, 8.8, 8.8, 1.4)),
            detail([(15.6, 15.6), (20.8, 15.6)], 1.0), detail([(15.6, 17.6), (20.8, 17.6)], 1.0),
            detail([(15.6, 19.6), (20.8, 19.6)], 1.0)]


def badge_plus():
    return [fill(circle(18.0, 18.0, 4.4)), detail([(15.8, 18.0), (20.2, 18.0)], 1.4),
            detail([(18.0, 15.8), (18.0, 20.2)], 1.4)]


def badge_link():
    return [fill(circle(18.0, 18.0, 4.4)),
            detail([(16.0, 20.4), (16.1, 19.0), (16.7, 17.6), (17.8, 16.8), (19.6, 16.6)], 1.3),
            detail([(18.6, 15.2), (20.0, 16.6), (18.6, 18.0)], 1.2)]


def forbidden(cx, cy, r, width):
    d = r / math.sqrt(2)
    return [line(arc(cx, cy, r, 0, 360, 40), width), line([(cx - d, cy - d), (cx + d, cy + d)], width)]


def lens(sign):
    parts = [glass(circle(10, 10, 5.4, 40)), line(arc(10, 10, 5.8, 0, 360, 40), 2.0),
             line([(14.4, 14.4), (20.2, 20.2)], 3.0), line([(7.4, 10), (12.6, 10)], 1.6)]
    if sign == "+":
        parts.append(line([(10, 7.4), (10, 12.6)], 1.6))
    return parts


def fleur():
    return [line([(12, 5.5), (12, 18.5)], 1.6), line([(5.5, 12), (18.5, 12)], 1.6),
            fill(head(12, 2.0, -90)), fill(head(12, 22.0, 90)), fill(head(2.0, 12, 180)), fill(head(22.0, 12, 0))]


def four_heads():
    return [fill(head(12, 2.0, -90)), fill(head(12, 22.0, 90)), fill(head(2.0, 12, 180)), fill(head(22.0, 12, 0)),
            fill(circle(12, 12, 1.9, 20))]


def crosshair():
    return [line([(12, 3), (12, 9.3)], 1.6), line([(12, 14.7), (12, 21)], 1.6), line([(3, 12), (9.3, 12)], 1.6),
            line([(14.7, 12), (21, 12)], 1.6), fill(circle(12, 12, 1.1, 16))]


def cell():
    a, b = 2.2, 8.5
    return [fill([(12 - a, 12 - b), (12 + a, 12 - b), (12 + a, 12 - a), (12 + b, 12 - a), (12 + b, 12 + a),
                  (12 + a, 12 + a), (12 + a, 12 + b), (12 - a, 12 + b), (12 - a, 12 + a), (12 - b, 12 + a),
                  (12 - b, 12 - a), (12 - a, 12 - a)])]


# ---- the set ----

CENTER = (12.0, 12.0)

CURSORS = {
    "default": ([arrow()], ARROW_HOT),
    "pointer": ([hand_point()], (9.75, 2.2)),
    "grab": ([hand_open()], CENTER),
    "grabbing": ([hand_fist()], CENTER),
    "text": ([ibeam()], CENTER),
    "crosshair": ([crosshair()], CENTER),
    "move": ([fleur()], CENTER),
    "all-scroll": ([four_heads()], CENTER),
    "not-allowed": ([forbidden(12, 12, 7.6, 2.6)], CENTER),
    "wait": ([[line(arc(12, 12, 7.0, -90, 180), 3.0)]], CENTER),
    "progress": ([arrow(), badge_spinner()], ARROW_HOT),
    "help": ([arrow(), badge_question()], ARROW_HOT),
    "context-menu": ([arrow(), badge_menu()], ARROW_HOT),
    "copy": ([arrow(), badge_plus()], ARROW_HOT),
    "alias": ([arrow(), badge_link()], ARROW_HOT),
    "no-drop": ([arrow(), forbidden(18.0, 18.0, 3.6, 1.6)], ARROW_HOT),
    "ew-resize": ([[fill(double_arrow())]], CENTER),
    "ns-resize": ([[fill(rot(double_arrow(), 90))]], CENTER),
    "nwse-resize": ([[fill(rot(double_arrow(length=18.0), 45))]], CENTER),
    "nesw-resize": ([[fill(rot(double_arrow(length=18.0), -45))]], CENTER),
    "col-resize": ([split_arrows()], CENTER),
    "row-resize": ([[(p[0], rot(p[1], 90)) for p in split_arrows()]], CENTER),
    "zoom-in": ([lens("+")], (10.0, 10.0)),
    "zoom-out": ([lens("-")], (10.0, 10.0)),
    "cell": ([cell()], CENTER),
}

ALIASES = {
    "n-resize": "ns-resize", "s-resize": "ns-resize", "e-resize": "ew-resize", "w-resize": "ew-resize",
    "ne-resize": "nesw-resize", "sw-resize": "nesw-resize", "nw-resize": "nwse-resize", "se-resize": "nwse-resize",
}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "user", "share",
                                                              "cursors")
    os.makedirs(out, exist_ok=True)
    index = ["# The cursors of X12, written by tools/gencursors.py (docs/design/cursors.md).",
             "# NAME X Y: the hotspot in units of the %d by %d view box. NAME = TARGET: an alias." % (SIZE, SIZE)]
    for name, (groups, (hx, hy)) in CURSORS.items():
        with open(os.path.join(out, name + ".svg"), "w") as f:
            f.write(svg(groups))
        index.append("%s %s %s" % (name, fmt(hx * SCALE), fmt(hy * SCALE)))
    for name, target in ALIASES.items():
        index.append("%s = %s" % (name, target))
    with open(os.path.join(out, "index"), "w") as f:
        f.write("\n".join(index) + "\n")


if __name__ == "__main__":
    main()
