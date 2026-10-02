# Images, screenshots, the image viewer and paint

This document describes the image functions of libgui, the screen
capture interface of X12, and the three programs built on them:
`screenshot`, the image viewer `view` and the bitmap editor `paint`.

## PNG decoding

`image_decode` in `libgui/src/png.c` reads every colour type of the PNG
format at every bit depth the format allows: grey at 1, 2, 4, 8 and 16
bits, palette images at 1, 2, 4 and 8 bits, and RGB, grey with alpha and
RGBA at 8 and 16 bits. Samples below 8 bits are scaled to 8 bits by
`v * 255 / (2^depth - 1)`, and 16 bit samples are rounded to 8 bits. The
transparent colour of a `tRNS` chunk is compared with the samples before
scaling. Adam7 interlaced files are decoded pass by pass: the data of the
seven passes follows one another in the zlib stream, each pass has its
own filter rows, and an empty pass has no rows. Images are limited to
16384 pixels in each dimension. The result is an RGBA image with straight
alpha (`0xAARRGGBB`).

## PNG encoding

`image_encode_png` in `libgui/src/pngenc.c` writes 8 bit RGB when every
pixel is opaque and 8 bit RGBA otherwise. Each row is filtered with the
filter whose output has the smallest sum of absolute values when the
bytes are read as signed numbers, which is the heuristic of the PNG
specification. The filtered data is compressed into a zlib stream of one
deflate block with the fixed Huffman codes. Matches are found with hash
chains over three byte prefixes in a 32 KiB window, at most 48
candidates per position, and the longest match is taken without lazy
evaluation. `image_save_png` writes the file. The 1024x768 screenshot of the
plain desktop in the boot test is 18 KB. Dynamic Huffman codes would make files with
varied content smaller and are not implemented.

## Resampling and drawing

`image_create` allocates a transparent image. `image_scale` resamples an
image: each destination pixel of a reduction is the average of the source
pixels its area covers, weighted by alpha, and an enlargement repeats the
nearest source pixel. `painter_image_scaled` draws an image into a
rectangle of logical pixels with nearest neighbour sampling and visits
only the device pixels inside the clip, so the cost depends on the
visible area and not on the size of the image.

## Screen capture

The `screencopy` global of `protocol/debug.xml` copies the screen into a
shared memory buffer of the client. Binding it sends `size` with the
screen in device pixels and the output scale, and X12 sends `size` again
after a mode change (`debug_screen_changed`). `capture(buffer)` copies the
back buffer, which contains the last composed frame and the pointer, into
the buffer with opaque alpha and answers `done`, or `failed` when the
buffer has another size. The copy is made while the request is decoded,
so X12 never refers to a buffer after the request.

`/bin/screenshot` (`user/apps/screenshot.c`) connects through libgui,
binds `shm` and `screencopy`, creates a buffer of the reported size,
requests the copy and saves the buffer with `image_save_png`. Without a
file argument it writes `$HOME/Pictures/screenshot-YYYY-MM-DD-HHMMSS.png`
and creates the directory. It prints the path on standard output. The
time is UTC, because the libc has no time zones.

The launcher menu has a Screenshot entry, and X12 starts
`/bin/screenshot` through `mime_spawn` when Print Screen (`KEY_SYSRQ`) is
pressed without Alt. Alt+SysRq is the thread table of
the kernel (`debug.md`) and is passed to the focused client unchanged.

## Image viewer

`view` (`user/apps/view.c`, package `view`) shows PNG and SVG files. The
`view` package registers it for `image/*`, so Files and the desktop open
PNG files with it. A file argument opens the file and lists the other
image files of its directory, sorted by name. A directory argument opens
its first image. SVG files are rendered at 1024 by 1024 pixels by the
SVG renderer of the icons.

The zoom is a number of device pixels per 1000 image pixels. Zoom 0 fits
the image into the window and never enlarges it. The zoom steps are 5 %
to 1600 %. A zoom below 100 % draws a copy reduced by `image_scale`,
made once per zoom and window size at device resolution. A zoom of
100 % or more draws the decoded image with `painter_image_scaled`.
Transparent images are drawn over a grey checkerboard. A zoom change
leaves the point at the centre of the view at the centre. The wheel scrolls, Shift with the
wheel scrolls sideways, and Ctrl with the wheel zooms. libgui reports the
modifier keys for mouse events through `gui_modifiers`. The status bar
shows the file name, the image size, the zoom and the position in the
directory.

File, Set as wallpaper runs `settings set wallpaper PATH`, which writes
the configuration file that the desktop client reads. It is enabled for
PNG files only, because the desktop decodes PNG wallpapers only.

## Paint

`paint` (`user/apps/paint.c`, package `paint`) edits a drawing of one
pixel per logical pixel, inside a scroll area. The tools are the brush,
the eraser (white), lines, rectangles and ellipses, outlined with the
brush size or filled, and a scanline flood fill. A shape is drawn again
on every pointer motion from the copy of the drawing taken at the press,
and the release makes that copy the undo step. Undo and redo store whole
copies of the drawing, at most eight in each direction. The palette row
below the drawing shows the current colour and sixteen colours.

New asks for a size up to 4096x4096. Open composes a PNG file over white,
because the drawing has no alpha channel. Save writes opaque RGB with
`image_save_png`. The window title shows the file name and an asterisk
while the drawing has unsaved changes. Closing the window, Quit, New and
Open ask whether to save such changes.

## Tests

`make check` runs the host tests of libgui. `libgui/tests/test_images.c`
decodes files written by `tools/genicons/genicons.py`: 4 bit grey, a 2
bit palette, 16 bit RGBA, interlaced RGBA and interlaced 1 bit grey, each
compared with the formula that generated it. It encodes an opaque and a
translucent image, checks the colour type and decodes both back to the
same pixels, checks that a uniform 1024x768 image compresses below 40000
bytes, and checks `image_scale` and `painter_image_scaled`.

The boot test `gui_images` runs `screenshot /shot.png` and checks the
PNG header against the framebuffer size, presses Print Screen, opens the
screenshot in the viewer and counts the desktop colour and the
background in the window, and presses the zoom and navigation keys. In
paint it draws a stroke, undoes and redoes it, saves `/drawing.png` with
Ctrl+S, and opens the saved file in the viewer to count the black pixels
of the stroke. The post script runs `fsck -y` on the disk image, extracts
both files with `mkfs --cat`, decodes them with an independent decoder in
Python, and checks the screenshot size, its desktop colour and the
pixels of the drawing.
