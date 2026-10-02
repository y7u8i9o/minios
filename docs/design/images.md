# Images, screenshots, the image viewer and paint

This document describes the image functions of libgui, the screen
capture interface of X12, and the three programs built on them:
`screenshot` with its capture interface, the image viewer `view` and the
bitmap editor `paint`.

## PNG decoding

The PNG codec is the module `png.so` of libcodec
(`libcodec/modules/png/`, `codecs.md`); libgui's `image_decode` and
`image_load` reach it through the registry, which picks it by the PNG
signature. The decoder (`decode.c`) reads every colour type of the PNG
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

The encoder (`libcodec/modules/png/encode.c`, behind libgui's
`image_encode_png`) writes 8 bit RGB when every
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
after a mode change (`debug_screen_changed`). `capture(buffer, pointer)`
copies the back buffer, which contains the last composed frame, into the
buffer with opaque alpha and answers `done`, or `failed` when the buffer
has another size. With `pointer` 1 the copy contains the pointer, and the
`pointer` event reports its rectangle in device pixels before `done`
(empty while the pointer is hidden). With 0 `scene_copy_screen` composes
the rectangle under the pointer again with the cursor suppressed, copies
the back buffer, and composes the rectangle once more with the cursor.
Neither composition is flushed, so the screen never shows the frame
without the pointer. Every copy is made while the request is decoded, so
X12 never refers to a buffer after the request.

`get_windows` sends a `window` event for every mapped toplevel that is
not minimized, from the top of the stack down, with its number, its
visible frame in logical pixels, whether it is activated, its title, and
the size of its image in device pixels, followed by `windows_done`. The
image covers the extent of the window: the frame and the shadow drawn by
X12, or the whole surface of a window with client side decorations,
whose shadow is part of the surface. `capture_window(buffer, number)`
draws that window alone into an ARGB buffer of the image size
(`scene_render_window`). The back buffer is pointed at a scratch target
and the surface is moved so that its extent starts at the origin; the
shadow, the decorations and the surface are drawn once over black into
the client buffer and once over white into a copy, and the surface is
moved back. A pixel that came out the same over both backgrounds is
opaque. Otherwise the largest difference of a channel is the part of the
background that shows through, so the alpha is 255 minus that
difference and the colour is the pixel over black divided by the alpha.
The image therefore has the soft shadow and the rounded corners on a
transparent background, and parts of other windows that cover the window
on the screen do not appear in it. Popups of the window are not drawn.

## The screenshot program

`/bin/screenshot` (`user/apps/screenshot.c`) connects through libgui
and binds `shm` and `screencopy`. Its modes follow `gnome-screenshot`:

    screenshot [-i | -a | -w] [-p] [-t] [-d SECONDS] [FILE]

Without a mode it copies the screen into a buffer of the reported size,
with the pointer when `-p` is given, and saves it. `-w` saves the
activated window, or the top window when none is activated, through
`capture_window`. `-i` opens the capture interface and `-a` the area
selection described below. `-d` waits before the copy. `-t` shows the
thumbnail after the file is written; `-i` and `-a` show it always. The
image is saved with `image_save_png`; a window image has an alpha
channel, so its file is RGBA. Without a file argument the program writes
`$HOME/Pictures/screenshot-YYYY-MM-DD-HHMMSS.png` in local time and
creates the directory. It prints the path on standard output, and a
cancelled capture prints nothing and exits with status 1. Messages on
standard output and standard error stay in English; the labels of the
toolbar are translated in the `screenshot` domain.

## The capture interface

The interface of `-i` follows the screenshot interface of GNOME. The
program first freezes the screen: it captures it once with the pointer
and keeps the pixels of the reported pointer rectangle, captures it again
without the pointer into the same buffer, and asks for the window list.
It then maps a layer surface of the screen size on the overlay layer
with keyboard interactivity, which X12 places above the panel and its
popups and gives the keyboard focus when it maps. The surface shows the
frozen screen at half brightness and the part that will be saved at full
brightness, so live changes of the screen behind it do not show.

The toolbar is centred 24 pixels above the bottom edge and is drawn in
the colours of the theme. It has the Selection, Screen and Window mode
buttons with an icon over the label, the capture button (an accent ring
around an accent disc), the pointer toggle and Close. In Selection mode
the selection starts as the middle half of the screen and is drawn with a
white outline, round handles at its corners and the middles of its
edges, and a label with its size in device pixels below it. Dragging
outside the selection draws a new one once the pointer has moved four
pixels, so that a click keeps the old selection; dragging inside moves
it within the screen, and dragging a handle or an edge moves the edges it
grabs. Screen mode shows the whole screen bright. Window mode shows the
activated window bright with an accent frame and outlines the window
under the pointer in white; a click chooses a window. The keys are those
of GNOME: S, C and W select the modes, P toggles the pointer, Enter, Space
and Print Screen capture, and Escape cancels.

When the capture is chosen the program destroys the surface, waits for a
round trip so that the screen is back before the file is encoded, and
cuts the selection or the screen out of the frozen copy. With the
pointer toggle on, the kept pointer pixels are copied over the part of
the cut that they cover. A window is captured again through
`capture_window` after a fresh window list, because the window may have
changed its size while the interface was shown, so the pointer is never
part of a window image.

`-a` is the area selection of macOS: the same surface without the
toolbar, the handles and an initial selection. A drag draws the
selection, and releasing the button saves it. Space switches between the
area and the window under the pointer, which a click saves; Escape
cancels.

## Thumbnail and keys

After a capture with `-t`, `-i` or `-a` the program shows the saved image
as on macOS: a layer surface on the overlay layer without keyboard
interactivity, anchored to the bottom right corner with margins of 16
pixels (`gui_layer_set_margin`), so that it stays above the panel. It
holds the image scaled into at most 220 by 140 pixels inside a one
pixel light grey frame. A click opens the file with `mime_open`, which
starts the image viewer, and the thumbnail goes after five seconds,
counted again whenever the pointer moves over it.

X12 starts the program through `mime_spawn` on the keys of both systems.
Print Screen (`KEY_SYSRQ`) starts `screenshot -i` and Shift+Print Screen
`screenshot -t`, as in GNOME. Super+Shift+3, Super+Shift+4 and
Super+Shift+5 start `screenshot -t`, `screenshot -a` and `screenshot -i`,
as on macOS. The keys are kept from the focused client, and their
releases as well. While an overlay layer surface has the keyboard focus,
Print Screen is passed to it as its capture key and the other keys start
nothing. Alt+SysRq is the thread table of the kernel (`debug.md`) and is
passed to the focused client unchanged. The launcher menu has a
Screenshot entry that starts `screenshot -i`.

## Image viewer

`view` (`user/apps/view.c`, package `view`) shows every image format a
codec module decodes (`codecs.md`): PNG, BMP and SVG files. The `view`
package registers it for `image/*`, so Files and the desktop open image
files with it. A file argument opens the file and lists the other files
of its directory whose extension a codec decodes, sorted by name. A
directory argument opens its first image. Each file is decoded by the
codec its content names; vector formats are rendered at 1024 by 1024
pixels, SVG by the renderer of the icons.

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
raster formats, the codecs without `CODEC_SCALABLE`, since the desktop
decodes its wallpaper with `image_load` at the file's own size.

## Paint

`paint` (`user/apps/paint.c`, package `paint`) edits a drawing of one
pixel per logical pixel, inside a scroll area. The tools are the brush,
the eraser (white), lines, rectangles and ellipses, outlined with the
brush size or filled, and a scanline flood fill. A shape is drawn again
on every pointer motion from the copy of the drawing taken at the press,
and the release makes that copy the undo step. Undo and redo store whole
copies of the drawing, at most eight in each direction. The palette row
below the drawing shows the current colour and sixteen colours.

New asks for a size up to 4096x4096. Open composes an image of any
decodable format over white, because the drawing has no alpha channel.
Save writes the opaque drawing in the format of the file name's
extension through `codec_image_save`, and as PNG when no codec encodes
that extension. The window title shows the file name and an asterisk
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
PNG header against the framebuffer size. It presses Print Screen and
checks that the desktop is dimmed beside the selection and bright inside
it, presses Enter, and finds the frame of the thumbnail 16 pixels from
the right edge and the panel. It presses Super+Shift+4, drags an area
and checks that the area is bright before the button goes up. It opens
the screenshot in the viewer, counts the desktop colour and the
background in the window, and presses the zoom and navigation keys. With
the viewer open it presses Print Screen and W and checks that the window
stays bright while the desktop beside it is dimmed, presses Enter, and
runs `screenshot -w /win.png`, whose header must be RGBA and the size of
the window with its shadow. In paint it draws a stroke, undoes and redoes
it, saves `/drawing.png` with Ctrl+S, and opens the saved file in the
viewer to count the black pixels of the stroke. The post script runs
`fsck -y` on the disk image, extracts the files with `mkfs --cat`,
decodes them with an independent decoder in Python, and checks the
screenshot size and its desktop colour, the transparent corner and the
opaque middle of the window image, and the pixels of the drawing. Started
with `hold=1` on the kernel command line, the test waits eight seconds
in the capture interface and in its window mode for screen dumps.
