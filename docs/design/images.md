# Images, screenshots, the image viewer and paint

This document describes the image functions of libgui, the screen
capture interface of X12, and the three programs built on them:
`screenshot` with its capture interface, the image viewer `view` and the
bitmap editor `paint`.

## PNG decoding

The PNG codec is the libcodec module `png.so` (`lib/libcodec/modules/png/`,
`codecs.md`). The libgui functions `image_decode` and `image_load` reach
it through the codec registry, whose lookup selects `png.so` because the
PNG probe matches the eight byte signature. The decoder (`decode.c`) reads every colour type of the PNG
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

The encoder (`lib/libcodec/modules/png/encode.c`, behind libgui's
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
image one axis after the other with premultiplied alpha. An axis that
becomes shorter averages the source pixels that each destination pixel
covers, weighted by the covered length. An axis that becomes longer
interpolates linearly between the two nearest source pixels, measured
from the pixel centres. The function produces one destination row at a
time and resamples each source row horizontally into a cache of two
rows, so a large image needs no temporary copy of its own size. Until B1
of `docs/plan/desktop-panel.md` an enlargement repeated the nearest
source pixel. `painter_image_scaled` draws an image into a
rectangle of logical pixels with nearest neighbour sampling and visits
only the device pixels inside the clip, so the cost depends on the
visible area and not on the size of the image.

## Screen capture

The `screencopy` global of `protocol/debug.xml` copies the screen into a
shared memory buffer that the client provides. When a client binds the
global, X12 sends `size` with the screen size in device pixels and the
output scale, and it sends `size` again after every mode change
(`debug_screen_changed`). `capture(buffer, pointer)` copies the back
buffer, which contains the last composed frame, into the client buffer with
opaque alpha and answers `done`. It answers `failed` when the buffer has
a different size. With `pointer` set to 1 the copy includes the pointer,
and X12 reports the pointer rectangle in device pixels with a `pointer`
event before `done`. The rectangle is empty while the pointer is hidden.
With `pointer` set to 0, `scene_copy_screen` composes the area under the
pointer again without the cursor, copies the back buffer, and then
composes the area once more with the cursor. Neither composition is
flushed to the framebuffer, and the screen therefore never shows a frame
without the pointer. X12 makes every copy while it decodes the request
and retains no reference to the buffer afterwards.

`get_windows` sends one `window` event for every mapped toplevel that is
not minimized, starting at the top of the stack. Each event carries the
window number, the visible frame in logical pixels, the activation
state, the title and the image size in device pixels. A `windows_done`
event ends the list. The image covers the whole extent of the window,
which is the frame together with the shadow that X12 draws, or the whole
surface of a window with client side decorations, because such a window
draws its shadow into its own surface. `capture_window(buffer, number)`
draws that window alone into an ARGB buffer of the image size
(`scene_render_window`). X12 points the back buffer at a scratch target
and moves the surface until its extent starts at the origin. It draws
the shadow, the decorations and the surface twice, once over black into
the client buffer and once over white into a copy, and then moves the
surface back. A pixel with the same value over both backgrounds is
opaque. For any other pixel, the largest difference between the two
results in one channel is the amount of background that shows through.
The alpha is 255 minus that difference, and the colour is the pixel over
black divided by the alpha. The resulting image contains the soft shadow
and the rounded corners on a transparent background. Other windows that
cover the window on the screen do not appear in it. Popups that belong to
the window are not drawn.

## The screenshot program

`/bin/screenshot` (`user/apps/screenshot.c`) connects through libgui
and binds `shm` and `screencopy`. Its options follow `gnome-screenshot`:

    screenshot [-i | -a | -w] [-p] [-t] [-d SECONDS] [FILE]

Without a mode option the program copies the whole screen into a buffer
of the reported size and saves it, with the pointer when `-p` is given.
`-w` saves the active window through `capture_window`, or the top window
when no window is active. `-i` opens the capture interface and `-a` the
area selection, both described below. `-d` waits the given number of
seconds before the copy. `-t` shows a thumbnail after the file is
written, and `-i` and `-a` always show it. The image is encoded as PNG,
and a window image has an alpha channel, which makes its file RGBA.
Without a file argument the program writes
`$HOME/Pictures/screenshot-YYYY-MM-DD-HHMMSS.png` in local time and
creates the directory when necessary. It prints the path on standard
output. A cancelled capture prints nothing and exits with status 1.
Messages on standard output and standard error remain in English. The
toolbar labels are translated in the `screenshot` domain.

## The capture interface

The interface of `-i` is modelled on the screenshot interface of GNOME.
The program first freezes the screen. It captures the screen once with
the pointer and saves the pixels of the reported pointer rectangle, then
captures it again without the pointer into the same buffer, and finally
requests the window list. Next it maps a layer surface of the screen
size on the overlay layer with keyboard interactivity. X12 places this
surface above the panel and its popups and gives it the keyboard focus
when it maps. The surface shows the frozen screen at half brightness and
the area that will be saved at full brightness. Changes on the live
screen behind the surface are not visible.

The toolbar is centred 24 pixels above the bottom edge of the screen and
uses the colours of the theme. It contains the Selection, Screen and
Window mode buttons, each with an icon above its label, the capture
button (an accent ring around an accent disc), the pointer toggle and
Close. In Selection mode the initial selection covers the middle half of
the screen. The selection has a white outline, round handles at its
corners and at the middle of each edge, and a label below it that gives
its size in device pixels. Dragging outside the selection starts a new
selection after the pointer has moved four pixels, which leaves the old
selection in place after a plain click. Dragging inside the selection
moves it within the screen, and dragging a handle or an edge moves the
edges it contains. Screen mode shows the whole screen at full brightness.
Window mode shows the active window at full brightness with an accent
frame and outlines the window under the pointer in white. Clicking a
window selects it. The keys are those of GNOME. S, C and W select the
modes, P toggles the pointer, Enter, Space and Print Screen capture, and
Escape cancels.

When the user captures, the program destroys the surface and waits for a
round trip to make sure the live screen is visible again before the file
is encoded. It then cuts the selection or the screen out of the frozen
copy. If the pointer toggle is on, the saved pointer pixels are copied
over the part of the image they cover. A window is captured again
through `capture_window` after a new window list, because the window may
have changed size while the interface was shown. A window image never
contains the pointer.

`-a` selects an area as macOS does. It uses the same surface without the
toolbar, the handles and an initial selection. The user drags out the
selection, and releasing the button saves it. Space switches between the
area and the window under the pointer, and a click saves that window.
Escape cancels.

## Thumbnail and keys

After a capture with `-t`, `-i` or `-a`, the program shows the saved
image as macOS does. The thumbnail is a layer surface on the overlay
layer without keyboard interactivity. It is anchored to the bottom right
corner with margins of 16 pixels (`gui_layer_set_margin`), which places it
above the panel. It shows the image scaled to at most 220 by 140 pixels
inside a light grey frame one pixel wide. A click opens the file with
`mime_open`, which starts the image viewer. Otherwise the thumbnail
disappears after five seconds, and the five seconds start again whenever
the pointer moves over it.

X12 starts the program through `mime_spawn` on the keys of both systems.
Print Screen (`KEY_SYSRQ`) starts `screenshot -i` and Shift+Print Screen
starts `screenshot -t`, as in GNOME. Super+Shift+3, Super+Shift+4 and
Super+Shift+5 start `screenshot -t`, `screenshot -a` and `screenshot -i`,
as on macOS. X12 does not deliver these keys, or their releases, to the
focused client. While an overlay layer surface has the keyboard focus,
X12 passes Print Screen to it as its capture key and ignores the other
screenshot keys. Alt+SysRq prints the thread table of the kernel
(`debug.md`), and X12 passes it to the focused client unchanged. The
launcher menu has a Screenshot entry that starts `screenshot -i`.

## Image viewer

`view` (`user/apps/view.c`, package `view`) shows every image format that
a codec module decodes (`codecs.md`), which currently means PNG, BMP and
SVG files. The `view` package registers the viewer for `image/*`, and
Files and the desktop use it to open image files. With a file argument
the viewer opens that file and lists the other files in its directory
whose extension a codec can decode, sorted by name. With a directory
argument it opens the first image in the directory. The codec that
decodes a file is chosen by the file's content. Vector formats are
rendered at 1024 by 1024 pixels, and SVG files are rendered by the same
code as the icons.

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
the configuration file that the desktop client reads. The entry is
enabled only for raster formats, meaning codecs without
`CODEC_SCALABLE`, because the desktop decodes its wallpaper with
`image_load` at the size stored in the file.

## Paint

`paint` (`user/apps/paint.c`, package `paint`) edits a drawing of one
pixel per logical pixel, inside a scroll area. The tools are the brush,
the eraser (white), lines, rectangles and ellipses, outlined with the
brush size or filled, and a scanline flood fill. A shape is drawn again
on every pointer motion from the copy of the drawing taken at the press,
and the release makes that copy the undo step. Undo and redo store whole
copies of the drawing, at most eight in each direction. The palette row
below the drawing shows the current colour and sixteen colours.

New asks for a size up to 4096x4096. Open accepts any decodable format
and composes the image over white, because the drawing has no alpha
channel. Save writes the opaque drawing through `codec_image_save` in
the format that matches the extension of the file name, and in PNG when
no codec encodes that extension. The window title shows the file name and an asterisk
while the drawing has unsaved changes. Closing the window, Quit, New and
Open ask whether to save such changes.

## Tests

`make check` runs the host tests of libgui. `lib/libgui/tests/test_images.c`
decodes files written by `tools/genicons/genicons.py`: 4 bit grey, a 2
bit palette, 16 bit RGBA, interlaced RGBA and interlaced 1 bit grey, each
compared with the formula that generated it. It encodes an opaque and a
translucent image, checks the colour type and decodes both back to the
same pixels, checks that a uniform 1024x768 image compresses below 40000
bytes, and checks `image_scale` and `painter_image_scaled`.

The boot test `gui_images` runs `screenshot /shot.png` and checks the
PNG header against the framebuffer size. It then presses Print Screen,
checks that the desktop is dimmed outside the selection and at full
brightness inside it, presses Enter, and looks for the thumbnail frame
16 pixels from the right edge of the screen and from the panel. Next it
presses Super+Shift+4, drags out an area, and checks that the area is
at full brightness before it releases the button. It opens the
screenshot in the viewer, counts the pixels of the desktop colour and of
the window background, and presses the zoom and navigation keys. With
the viewer still open it presses Print Screen and W, checks that the
window remains at full brightness while the desktop beside it is dimmed,
and presses Enter. It then runs `screenshot -w /win.png`, whose header
must describe an RGBA image of the size of the window with its shadow.
In paint it draws a stroke, undoes and redoes it, and saves
`/drawing.png` with Ctrl+S. `codecs convert` turns the drawing into
`/drawing.bmp`, and the viewer opens the BMP file, in which the test
counts the black pixels of the stroke. The post script runs `fsck -y` on
the disk image, extracts the files with `mkfs --cat`, decodes them with
independent decoders written in Python, and checks the screenshot size
and its desktop colour, the transparent corner and the opaque centre of
the window image, the pixels of the drawing, and the identical pixels of
the BMP copy. When the kernel command line contains `pause=1`, the test
waits eight seconds in the capture interface and in its window mode to
allow screen dumps.
