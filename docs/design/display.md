# Display: virtio-gpu, virtio-input and run time modes (M32)

## The active framebuffer

`drivers/fbdev.c` owns `fb_screen`, the description of the framebuffer
everything draws into: address (in the direct map), size, pitch and pixel
layout, plus `fb_screen_scale`, the integer pixel scale. At boot
`fb_screen_init` copies the Limine framebuffer and the `@N` suffix of
`video=`. The console (`drivers/fbcon.c`), `/dev/fb0` and the kernel tests
read `fb_screen`; it changes only through `console_set_screen`, which takes
`console_lock`, replaces the description and re-initializes the console
grid, retaining the last rows of text when the grid shrinks. `/dev/fb0`
ioctls read it under `fb_mode_lock`.

A GPU driver registers `struct fb_gpu_ops` (prepare a mode, commit it,
flush a rectangle, flush by polling) with `fb_gpu_register`. `fb_flush`
is a no-op without a GPU, so user programs always call it. `fb_set_mode`
validates the request, lets the driver create the new scanout resource,
switches the console under `console_lock`, commits the scanout and
flushes the whole screen. Without a GPU only the scale may change.

`struct fb_info` gained `caps` (`FB_CAP_FLUSH`, `FB_CAP_SET_MODE`) and
`size`, the number of bytes `mmap` may map. With mode setting the size is
the whole GPU buffer: the mapping remains valid across mode changes, only
the geometry reported by `FBIOGET_INFO` changes. `FBIO_FLUSH` takes a
`struct fb_rect`; `FBIO_SET_MODE` takes `struct fb_mode` and is allowed to
the display owner only. Device mappings of RAM (the GPU buffer) take one
page reference per mapped page, matching the release at unmap; the driver
has its own reference on every page, so the block is never freed and the
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
Flushing is `TRANSFER_TO_HOST_2D` followed by `RESOURCE_FLUSH` for each
rectangle (a request descriptor and a response descriptor each). Since
G4 of `docs/plan/compositor-performance.md` one flush call takes up to
`FB_FLUSH_MAX` (32) rectangles. The driver publishes the transfers of
all rectangles, notifies the device once and waits until all are
complete, then does the same for the flushes. The thread therefore
waits twice per call, whatever the number of rectangles, and the host
shows only transferred pixels. `virtq_publish` and `virtq_notify` are
the two halves of `virtq_submit` for such batches. When the queue has no
free descriptors, the driver notifies the device and waits for
completions before it publishes more.

`FBIO_FLUSH_RECTS` (`struct fb_flush_rects`, capability
`FB_CAP_FLUSH_RECTS`) passes such a set from user space. While a file
owns the display, only that file may call it (`EPERM`), because a flush
of another process could show a half composed frame of the owner. A
count above 32 or nonzero flags give `EINVAL`. `FBIO_FLUSH` with one
rectangle remains unchanged. The test `gpu_mode` draws three squares,
flushes two in one call and checks a screendump of the host with
`tests/ppm_pixels.py`. It runs on x86_64 and on aarch64.

The console cannot talk to the device while having acquired
`console_lock`, so it accumulates a dirty rectangle and the
`gpu_flushd` thread fetches it every 20 ms with `console_take_dirty` and
flushes it. While a process owns the display, the console reports no
dirty rectangle, and the acquisition discards a rectangle that is still
pending. Before G4 that rectangle was flushed up to 20 ms after the
acquisition, with the pixels of the owner, which the screendump of
`gpu_mode` showed on aarch64. The panic path (`fb_panic_flush`) transfers the whole screen
by polling the used ring with a preallocated request, skipping the flush
when the queue lock is already locked.

## virtio-input

`drivers/virtio/virtio_input.c` accepts virtio-input devices (0x1052):
tablets, mice and, since M47, keyboards. The event queue contains 32
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
retain logical sizes (`gui_window.width`, `height`) and get a `scale`; the
surface and the two shared memory buffers are `width * scale` by
`height * scale` device pixels and the surface carries
`set_buffer_scale(scale)`. `gui_damage` takes logical rectangles, damage
is tracked in device pixels and reported to the compositor in logical
pixels rounded outwards. When the output's `done` event reports another
scale (a mode change from Settings > Display), every window is
re-created at the new scale and receives `WM_RESIZED`.

`struct painter` gained `scale`: local coordinates remain logical, the
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

The compositor's back buffer contains device pixels. Damage, surface
positions, input and the shell remain logical; `compose_rect` converts
each rectangle with `rect_scale`. `draw_surface` copies a buffer whose scale
equals the screen scale row by row and resamples other buffers (nearest
neighbour through the buffer scale and transform), so an unscaled client
is doubled and a scaled one is sharp. The cursor is an ARGB image at the
screen scale, shadows are computed in device pixels, and
`decor_draw` paints frames, buttons and the title through a painter at
the screen scale. Since G7 of `docs/plan/compositor-performance.md` the
rounded corners and the button discs come from the coverage tables of
`gui/pixel.h` (`pixel_corner_table`, `gfx_disc`), and every toplevel
stores its title shaped at the screen scale (`gfx_text_shape`), shaped
again only after a change of the title or of the scale. No square root
or float blend runs per pixel. `backend_present` flushes the device rectangles of a
frame in one `FBIO_FLUSH_RECTS` request. On a device with
`FB_CAP_FLUSH` and the native format, which virtio-gpu offers, the back
buffer is the framebuffer mapping itself: the device shows nothing
until the flush, so X12 composes there and copies nothing (G5 of
`docs/plan/compositor-performance.md`). On std VGA and ramfb, which the
display scans continuously, X12 composes into a private back buffer,
and `backend_present` copies the device rows of the rectangles into the
framebuffer, packed into its format when that is not native.

Buffers grow four times at scale 2: a 960x600 terminal needs an 18 MiB
double buffered pool, so the per object limit of anonymous shared memory
(`SHM_MAX_PAGES` in `ipc/shm.c`) is 64 MiB, enough for a 2560x1600
window at scale 2; libgui reports a failed pool allocation on stderr
instead of leaving the window without buffers. `gui_term_scale2` runs
the terminal test at `video=2048x1536@2`; the GUI tests read pixels in
logical coordinates (`pixel` scales by `fb_screen_scale`) so they run at
any scale.

A run time mode change sends every layer surface a configure while
clients may still send a frame that they drew before the configure. The commit rule in
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

## The window size of the host (V3 of the 0.6.0 release)

QEMU sends a display event when the window of the virtual display changes
its size. The cocoa and gtk windows send it when the user resizes them,
and a VNC client sends the message `SetDesktopSize`. The device sets the
bit `VIRTIO_GPU_EVENT_DISPLAY` in `events_read` of its configuration space
and raises the configuration interrupt.

The virtio core (`virtio.c`) routes the configuration interrupt to the
vector of the queues. The handler compares `config_generation` of the
common configuration with the last generation it saw. A new generation
calls `config_changed` of the driver under `irq_lock`. The GPU driver
clears the event with `events_clear` and sets the atomic flag
`display_event`. The handler sends no control request. The thread
`gpu_flushd` takes the flag every 20 ms, requests the display
information and reports the preferred size of scanout 0 with
`fb_display_changed`.

`fbdev.c` records the last request (`struct fb_display`: width, height
and a serial) under `fbdev_lock` and logs `the host display requests
WxH`. `/dev/fb0` then reports `POLLIN` to the display owner. The ioctl
`FBIOGET_DISPLAY` returns the request and marks the serial read for the
owner. `FBIO_ACQUIRE` marks no request read. A display server that starts
after a request therefore learns it at once. The kernel console does not
follow the requests. The `display` node of `/dev/devices` shows the last
request as `host_request`.

X12 polls `/dev/fb0` with its clients. `comp_follow_display` reads the
request and applies it with `comp_set_mode` when the setting
`display_follow` is 1. The scale is the scale of the last mode that a
client or the boot chose (`chosen_scale`). The backend reduces the scale
of a mode below 640x480 logical pixels to 1. A later larger request
returns to the chosen scale. A request that equals the current mode
changes nothing. A request larger than the 16 MiB buffer is refused with
a log line. X12 sends the new `display_mode` to every settings client.

`display_follow` is 1 by default. The desktop forwards the key
`display_follow` of `desktop.conf`. The Display page of Settings shows it
as the check box "The resolution follows the size of the window of the
virtual machine". A change of the setting to 1 applies the last request
at once.

The panel requests a layer surface of width 0 with the anchors left,
right and bottom. The compositor then configures the panel with the width
of the screen after every mode change. Before V3 the panel requested the
width of the boot mode, and it remained at that width after a mode
change.

`gpu_resize` (`kernel/tests/test_display.c`) runs with virtio-vga on
x86_64 and with virtio-gpu-pci on aarch64. It starts X12, the panel and the desktop at 1024x768. The case has a `vnc`
file, and the runner attaches `-vnc unix:SOCKET`. The QMP script sends
`SetDesktopSize` for 1600x1000 and for 1280x720 as a VNC client
(`vnc-size` of `tests/qmp_input.py`) and takes a screendump after each
mode change. The test requires each mode and the panel colour at both
ends of the bottom row. It then sets `display_follow` to 0 with
`settings set`. A third request for 1440x900 arrives in the kernel and
leaves the mode at 1280x720. The `post` script checks the size of both
screendumps and the panel colour at both ends of their bottom rows.
`vnc-size` reads the answer of QEMU and fails unless the status is 0 or 4
(request forwarded). A VNC display shows console 0. The runner therefore
attaches virtio-gpu-pci before ramfb on aarch64, as `tools/run.sh` does.
With ramfb first, QEMU answered status 3 (invalid screen layout), because
ramfb accepts no size request.
