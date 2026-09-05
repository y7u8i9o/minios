# Display: virtio-gpu, virtio-input and run time modes (M32)

## The active framebuffer

`drivers/fbdev.c` owns `fb_screen`, the description of the framebuffer
everything draws into: address (in the direct map), size, pitch and pixel
layout, plus `fb_screen_scale`, the integer pixel scale. At boot
`fb_screen_init` copies the Limine framebuffer and the `@N` suffix of
`video=`. The console (`drivers/fbcon.c`), `/dev/fb0` and the kernel tests
read `fb_screen`; it changes only through `console_set_screen`, which takes
`console_lock`, replaces the description and re-initializes the console
grid, keeping the last rows of text when the grid shrinks. `/dev/fb0`
ioctls read it under `fb_mode_lock`.

A GPU driver registers `struct fb_gpu_ops` (prepare a mode, commit it,
flush a rectangle, flush by polling) with `fb_gpu_register`. `fb_flush`
is a no-op without a GPU, so user programs always call it. `fb_set_mode`
validates the request, lets the driver create the new scanout resource,
switches the console under `console_lock`, commits the scanout and
flushes the whole screen. Without a GPU only the scale may change.

`struct fb_info` gained `caps` (`FB_CAP_FLUSH`, `FB_CAP_SET_MODE`) and
`size`, the number of bytes `mmap` may map. With mode setting the size is
the whole GPU buffer: the mapping stays valid across mode changes, only
the geometry reported by `FBIOGET_INFO` changes. `FBIO_FLUSH` takes a
`struct fb_rect`; `FBIO_SET_MODE` takes `struct fb_mode` and is allowed to
the display owner only. Device mappings of RAM (the GPU buffer) take one
page reference per mapped page, matching the release at unmap; the driver
holds its own reference on every page, so the block is never freed and the
swap daemon (which only takes frames referenced exactly once) never
touches it.

## virtio-gpu

`drivers/virtio/virtio_gpu.c` drives QEMU's `virtio-vga` (device id
0x1050; the VGA half lets Limine set a boot mode as before). At boot it
asks for the host's display info, allocates one 16 MiB block
(`PMM_MAX_ORDER` was raised to 12 for it), creates a 2D resource in the
`B8G8R8X8` format, which is the console's native `0x00RRGGBB` word
layout, attaches the block as backing, and registers itself. The initial
mode is the `video=` size when given (Limine may not have offered it: the
VGA BIOS has no 2048x1536, the GPU accepts any size up to 2560x1600),
else the Limine mode, else the host's preferred size.

A mode change creates a second resource over the same block, and the
scanout switches to it once the console has been re-initialized for the
new pitch, so the buffer is never interpreted with mixed geometries.
Flushing a rectangle is `TRANSFER_TO_HOST_2D` followed by
`RESOURCE_FLUSH`, each a control queue round trip (a request descriptor
and a response descriptor). The console cannot talk to the device while
holding `console_lock`, so it accumulates a dirty rectangle and the
`gpu_flushd` thread fetches it every 20 ms with `console_take_dirty` and
flushes it. The panic path (`fb_panic_flush`) transfers the whole screen
by polling the used ring with a preallocated request, skipping the flush
when the queue lock is held.

## virtio-input

`drivers/virtio/virtio_input.c` accepts virtio-input devices (0x1052):
tablets, mice and, since M47, keyboards. The event queue holds 32
posted 8 byte event buffers, each re-posted from its completion
callback; every event is reported unchanged to the input core
(`input.md`), which delivers it through `/dev/input/eventN`. The
capabilities and axis ranges come from the configuration space.
`virtio_input_feed` injects events for tests, through the first pointer
device or a virtual tablet.

With a tablet attached QEMU's window no longer grabs the mouse: the host
pointer position is delivered as is. `tools/run.sh` attaches
`virtio-tablet-pci` unless `--no-tablet` is given and uses `-vga virtio`
unless `--vga std` is given.

## The compositor

The framebuffer backend maps `fb_info.size`, flushes every rectangle it
copies (scaled to device pixels) and offers `backend_set_mode`, which
re-reads the geometry and reallocates the back buffer. `comp_set_mode`
applies a mode: the cursor is clamped, `shell_output_changed` repositions
layer surfaces, reconfigures those sized to the free area or stretched
across the screen, re-maximizes maximized windows and clamps the others,
and `output_changed` re-announces geometry, mode, scale and `done` to
every bound `output` resource. Absolute pointer events place the cursor
at `ax * width / 32768`.

The `display_mode` setting of the `settings` interface packs the mode as
`scale << 28 | width << 14 | height`. The desktop reads
`display_mode=WxH[@S]` from `/etc/desktop.conf` and forwards it, so a mode
chosen once persists across sessions; the settings application's Display
page offers common resolutions and the 1x/2x pixel scale. The kernel
command line `video=` remains the boot and console mode.

## Tests

- `gpu_mode` (virtio-vga): the driver took over at boot with a 16 MiB
  buffer, a kernel side mode change to 1600x1200@2 gives the console
  100x37 cells, an oversized mode is refused, and `fbmodetest` changes the
  mode through the ioctl (refused before acquiring the display), draws,
  flushes and restores the boot mode.
- `input_tablet` (virtio tablet attached): the device probes as an
  absolute pointer; injected absolute, button and wheel events arrive as
  the expected `/dev/input` events with its axis range.
- `gui_tablet`: the compositor draws the cursor where an absolute event
  points and repaints the old position.
- `comp_scale`, `gui` and `gui_desktop` run on virtio-vga; `comp_scale`
  boots `video=2560x1600@2`, a mode only the GPU can set.

## Toolkit side HiDPI (M33)

The output announces `scale` = the screen's pixel scale. libgui windows
keep logical sizes (`gui_window.width`, `height`) and get a `scale`; the
surface and the two shared memory buffers are `width * scale` by
`height * scale` device pixels and the surface carries
`set_buffer_scale(scale)`. `gui_damage` takes logical rectangles, damage
is tracked in device pixels and reported to the compositor in logical
pixels rounded outwards. When the output's `done` event reports another
scale (a mode change from Settings > Display), every window is
re-created at the new scale and receives `WM_RESIZED`.

`struct painter` gained `scale`: local coordinates stay logical, the
origin and clip are device pixels, fills, frames and lines are `scale`
pixels thick, rounded rectangles get a `scale` pixel border, images,
blits and masks are enlarged with nearest neighbour, and text is
rasterized at `px * scale` (outline fonts) or drawn as `scale` by
`scale` blocks (bitmap fonts) through `gfx_text_font_scaled`. Widths
returned to layout are logical (`ceil(device / scale)`), heights are the
logical font height, so layout does not depend on the scale. Widgets and
canvas paint handlers are unchanged; programs that write into
`gui_window.surf` directly must use its device size (`guitest` draws a
device checkerboard that way).

The compositor's back buffer holds device pixels. Damage, surface
positions, input and the shell stay logical; `compose_rect` converts
each rectangle with `dev()`. `draw_surface` copies a buffer whose scale
equals the screen scale row by row and resamples other buffers (nearest
neighbour through the buffer scale and transform), so an unscaled client
is doubled and a scaled one is sharp. The cursor shape is drawn in
`scale` by `scale` blocks, shadows are computed in device pixels, and
`decor_draw` paints frames, buttons and the title through a painter at
the screen scale. `backend_flush` copies device rows and flushes the
device rectangle.

Buffers grow four times at scale 2: a 960x600 terminal needs an 18 MiB
double buffered pool, so the per object limit of anonymous shared memory
(`SHM_MAX_PAGES` in `ipc/shm.c`) is 64 MiB, enough for a 2560x1600
window at scale 2; libgui reports a failed pool allocation on stderr
instead of leaving the window without buffers. `gui_term_scale2` runs
the terminal test at `video=2048x1536@2`; the GUI tests read pixels in
logical coordinates (`pixel` scales by `fb_screen_scale`) so they run at
any scale.

A run time mode change sends every layer surface a configure while
clients may still have a frame in flight. The commit rule in
`shell.c` therefore accepts a new buffer of the surface's old geometry
while a configure is unacknowledged, from either half of a client's
buffer pair; only a buffer of some third size is a protocol error. Before
this the desktop was dropped on the second mode change with "buffer
committed before configure acknowledgement" and every later change was
lost. `gui_modes` cycles 1024x768@1, 2560x1600@2 and 1920x1200@1 six
times with the panel, the desktop and the terminal running and prints
the free page count after each change; with libc's large blocks mapped
separately (`libc.md`) the count returns to its starting value whenever
the mode does.

`gui_scale2` boots `video=2048x1536@2` on virtio-vga: the compositor
starts at 1024x768 scale 2, guitest announces buffer scale 2, its device
checkerboard appears 1:1, beta's contents are at doubled coordinates, and
the title bar border and height are two and forty device rows.
