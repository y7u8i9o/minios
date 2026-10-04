# Icons

Icons are installed in `/usr/share/icons` under the names the programs use
(`new`, `open`, `save`, `cut`, `copy`, `paste`, `quit`, `folder`,
`file`, `terminal`, `clock`, `paint`, `pong`, `search`, `up`, `edit`,
`back`, `forward`, `home`, `refresh`, `zoom-in`, `zoom-out`, `fit`,
`undo`, `redo`, `line`, `rectangle`, `ellipse`, `fill`, `eraser`,
`wallpaper`, `stop`, `kill`, `profile`, and for the places of the folder
view `folder-new`, `drive`, `recent`, `documents`, `pictures`, `music`,
`videos` and `downloads`). The launcher menu of the panel
uses the icons `app-NAME`, where NAME is the file name of the program of
an entry, and `app-default` for programs without an icon. Since 2026-09-05 each name may
exist as an SVG file, which is preferred, and as a PNG file, which is
the fallback; both paths remain in the library.

## SVG icons (the `svg` codec)

The renderer is the libcodec module `svg.so`
(`lib/libcodec/modules/svg/svg.c`, `codecs.md`). The libgui function
`image_render_svg(text, len, px, color)` calls its decode function with a
request of px by px pixels. The renderer supports the subset of SVG that
icon files use: the `viewBox` (or `width` and `height`) of the `<svg>`
element, and every `<path>` with its `d` data, `fill` (`#rgb`,
`#rrggbb`, `none`, `black`, `white`; other values take `color`),
`fill-rule` (`nonzero` by default, `evenodd`), `fill-opacity` and
`opacity`. Comments, processing instructions, closing tags and other
elements are skipped. Path data supports `M L H V C S Q T A Z` and
their relative forms; cubic and quadratic curves are flattened into
segments in proportion to their size, arcs through the endpoint to
centre conversion of the specification. The view box is scaled to fit
a `px` by `px` square and centred, so Font Awesome's 448 and 576 wide
boxes retain their proportions. Edges are scan converted in 26.6 fixed
point with four sub rows per pixel and exact horizontal coverage, the
method of libfont's rasterizer, into an RGBA image with straight alpha;
several paths composite over each other. `image_load_svg(path, px,
color)` reads a file. Malformed data returns NULL with `EINVAL`.

`struct image` carries `scale`, the device pixels per logical pixel
(1 for PNG files). `painter_image` copies an image whose scale is the
painter's one to one and resamples other combinations by nearest pixel,
so a PNG icon is still doubled on a scale 2 output while an SVG icon
rendered at that scale is sharp. Widgets measure icons with `image_lw`
and `image_lh`, the logical size.

## The icon cache (`lib/libgui/src/widgets/icons.c`)

`icon_get(name)` looks for `<name>.svg` and renders it 16 logical
pixels high at the scale of the first output, then for `<name>.png`.
`icon_get_size(name, px)` renders an SVG icon at another logical size
(the desktop asks for 32) and returns NULL without an SVG, so the
desktop continues doubling PNG icons. Renders are cached per name and size
for the process. Font Awesome icons are single colour: the cache fills
them with the text colour `0x2a2a2a`, `folder` and `open` in amber
`0xd9a520` and `quit` in red `0xc04040`.

## Font Awesome

The SVG files are Font Awesome Free 6, solid style (CC BY 4.0, see
`third_party/fontawesome/NOTICE`). `tools/fetch_icons.sh` downloads
the twenty icons from the project's repository into
`third_party/fontawesome/svgs/<minios name>.svg`, using the table in
the script (for example `new` is `file-circle-plus`, `quit` is
`circle-xmark`, `refresh` is `arrows-rotate`); `make` copies the
directory into `/usr/share/icons` when it exists. The PNG icons drawn
in `tools/genicons/genicons.py` remain and are used for any name
without an SVG file.

## Tests

`lib/libgui/tests/test_svg.c` (part of `make check`) renders rectangles,
nested squares under both fill rules, circles from cubic curves and
from arcs, colour and opacity attributes, `tests/data/shape.svg` in
Font Awesome's file layout, refuses malformed input and paints a scale
2 image through a scale 2 painter.
