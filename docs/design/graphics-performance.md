# Graphics performance

This document describes the measurement of the graphical session and the
results of the compositor performance plan (`docs/plan/compositor-performance.md`).

## Clock and units

Times are microseconds of `CLOCK_MONOTONIC`. The libc function
`uptime_us()` reads them. The kernel derives the clock from `timer_ns`,
so the values have a resolution below one microsecond.

CPU time comes from `getrusage` and from the `TIME` column of `/dev/proc`.
The kernel charges a whole tick of one millisecond to the thread that runs
when the timer interrupt arrives. The frame timer of X12 expires on a
tick, so a frame that ends before the next tick is often not charged. The
CPU time of X12 is therefore too low when its frames are shorter than a
millisecond. The compose and flush times below have no such limit.

## Statistics of X12

`user/compositor/stats.c` counts the frames since the start of X12 or
since the last reset. A frame is one composition of the damage. Its time
runs from the start of the composition to the end of the last flush.

| Key | Meaning |
|---|---|
| `elapsed_us` | time since the reset |
| `frames` | compositions |
| `rects` | composed damage rectangles |
| `pixels` | composed device pixels, the area of the damage rectangles |
| `flushes` | presents of a frame (`backend_present`), before G5 one per rectangle |
| `flush_rects` | rectangles of the presents (since G5) |
| `flush_bytes` | bytes that the flushes copied into the framebuffer |
| `compose_us`, `compose_max_us` | time of the composition, total and the longest |
| `flush_us`, `flush_max_us` | time of the flushes, total and the longest |
| `frame_max_us` | the longest frame |
| `frame_p50_us`, `frame_p95_us`, `frame_p99_us` | percentiles of the frame time |
| `damage_latency_us`, `_max_us` | from the first damage after a frame to the end of the next frame |
| `commit_latency_us`, `_max_us` | from the earliest commit of a buffer to the end of the frame that shows it |
| `input_latency_us`, `_max_us` | from the earliest pointer motion to the end of the frame or of the device cursor move that shows it |
| `wakeups` | returns of `poll` in the main loop |
| `idle_timers` | frames without damage: before G6 the expirations of the periodic timer, since G6 frames that complete only frame callbacks |
| `cpu_us` | CPU time of X12, tick based (see above) |
| `back_bytes` | bytes of the back buffer |
| `pool_bytes` | bytes of the mapped client pools |
| `cursor_moves`, `cursor_us` | moves of the device cursor and their time (since G9). A move ends the input latency of the pointer motion. |

The flush time is the time spent in `backend_present`: the copy into the
framebuffer, which direct composition omits since G5, and the flush
ioctl. The compose time is the rest of
the frame. The percentiles come from a histogram with eight buckets per
power of two. A percentile is the middle of its bucket, within about six
percent of the exact value. A latency event that causes no composition is
forgotten at the next idle expiration of the frame timer. The averages
are the sum divided by the number of frames that ended a latency.

The `debug` interface of version 2 (`protocol/debug.xml`) sends the
values. `get_frame_stats` sends one `frame_stat` event with the key and
the high and low half of the 64 bit value for each key, then
`frame_stats_done`. `reset_frame_stats` sets everything to zero. The
command `compstat` prints the values on one line as `key=value` pairs,
`compstat -r` resets them, and `compstat -r -p` prints and then resets
them. Since X2 of `docs/plan/x12settings.md`, x12settings shows every
value and graphs of the last minute on its Performance page
(`x12settings.md`). X12 logs `slow frame: N us` for
a frame over 20 ms and the summary `frame stats` at exit.

## Statistics of libgui

`gui_get_stats()` (`gui/client.h`) reports the rendering of the windows of
the calling process since its start.

| Field | Meaning |
|---|---|
| `paints` | paints of a window by the framework that changed pixels |
| `paint_us` | time of these paints |
| `commits` | commits of a buffer |
| `copy_us`, `copied_bytes` | the copy into the shared buffer at a commit |
| `frame_waits` | commits deferred to a frame callback or a buffer release |
| `pool_bytes` | bytes of the shared buffer pools mapped now |
| `widget_paints` | calls of the paint functions of widgets |
| `painted_pixels` | device pixels of the damage of the window paints |
| `layouts` | measurements and layouts of single widgets |
| `text_shapes` | shapings of a text in an outline font |

The last four counters arrived with K2 of `docs/plan/widgets.md`. The
framework counts the paints and the layouts in `window.c` and
`layout.c` through `gui_count`. `font.c` counts each shaping. The
counters are deterministic for a given input, so the boot cases assert
them. Times are printed and never asserted.

## Benchmark

`compbench` (`user/tests/compbench.c`) opens a framework window with
760x540 logical pixels of contents. The window contains a canvas of 2x20
pixels at its top left corner and a canvas that fills the rest. It runs
one scenario:

- `blink`: the small canvas changes its colour 20 times, every 100 ms.
- `anim`: the large canvas changes its colour 60 times, every 16 ms.
- `idle`: nothing changes for two seconds.
- `window`: the window alone, until it is closed.

For the first three, compbench waits 800 ms, resets the statistics of
X12, runs the scenario, waits 300 ms and prints the values of X12 and
the values of `gui_get_stats` that the scenario added.

The boot test `kernel/tests/test_comp_bench.c` starts X12 alone and a
`compbench window`. It drives three scenarios and prints the values of
`compstat` after each:

- `drag`: 200 pointer packets on the title bar, 100 to the right and 100
  back.
- `pointer`: 400 absolute pointer motions over the window.
- `resize`: 20 drags of the bottom right corner, alternately 60x40
  pixels inwards and back. X12 applies a resize at the release.

It then runs the client scenarios `blink`, `anim` and `idle`. The case
`comp_bench` runs at 1280x800 and `comp_bench_hidpi` at 2560x1600 with
scale 2, both on virtio-gpu under TCG. The expect files require the
counters that do not depend on timing: 20 frames for `blink`, 0 frames
for `idle`, and the resize of the window.

`tools/benchtable.py` turns the serial logs of the two cases into the
tables below.

### Widget scenarios (K2 of `docs/plan/widgets.md`)

compbench has six scenarios of the widgets. Each scenario sends its
input to its own window with `window_message`, one step every 100 ms. The
counts therefore do not depend on the timing of the pointer.

- `wheel`: a table of 60 rows with 20 visible. 10 wheel steps up at the
  top, then 20 down. 14 of the 20 change the view, and 6 are at the end.
- `type`: 20 characters into a text field of 300 pixels.
- `status`: the label of a status bar is set 20 times.
- `edit`: 20 characters at line 3500 of an editor with 4000 lines of C
  and the highlighter.
- `scroll`: 20 wheel steps in a scroll area with 200 labels.
- `hover`: the pointer moves across 20 rows of the table.

The boot test `kernel/tests/test_gui_bench.c` starts X12 alone and runs
the six scenarios. The case `gui_bench` runs at 1280x800 and
`gui_bench_hidpi` at 2560x1600 with scale 2, both on virtio-gpu.
`tools/benchtable.py` prints a table of the counters of libgui for each
case.

## Baseline (G1, 2026-10-06)

The numbers come before any change of the plan. Times are milliseconds,
composed pixels are millions of device pixels.

1280x800 at scale 1:

| scenario | frames | composed Mpx | compose ms | flush ms | frame p50 ms | frame p95 ms | commit latency ms | input latency ms | wakeups | X12 CPU ms | client copy ms |
|---|---|---|---|---|---|---|---|---|---|---|---|
| drag | 69 | 32.99 | 149.6 | 69.3 | 2.9 | 4.4 | 0.0 | 14.6 | 290 | 96 |  |
| pointer | 400 | 0.20 | 32.1 | 140.6 | 0.4 | 1.1 | 0.0 | 8.4 | 1173 | 1 |  |
| resize | 100 | 18.44 | 86.8 | 46.4 | 0.3 | 3.2 | 12.1 | 8.9 | 1001 | 68 |  |
| blink | 20 | 9.54 | 52.8 | 22.3 | 3.5 | 4.9 | 13.6 | 0.0 | 168 | 41 | 4.0 |
| anim | 60 | 28.61 | 130.4 | 50.6 | 2.9 | 4.0 | 9.5 | 0.0 | 143 | 88 | 86.8 |
| idle | 0 | 0.00 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 148 | 0 | 0.0 |

2560x1600 at scale 2:

| scenario | frames | composed Mpx | compose ms | flush ms | frame p50 ms | frame p95 ms | commit latency ms | input latency ms | wakeups | X12 CPU ms | client copy ms |
|---|---|---|---|---|---|---|---|---|---|---|---|
| drag | 70 | 133.88 | 520.2 | 198.5 | 9.7 | 13.8 | 0.0 | 15.8 | 223 | 427 |  |
| pointer | 400 | 0.79 | 55.4 | 128.3 | 0.5 | 0.5 | 0.0 | 8.2 | 1166 | 0 |  |
| resize | 100 | 73.75 | 296.2 | 130.5 | 0.5 | 10.8 | 17.2 | 7.7 | 996 | 300 |  |
| blink | 20 | 38.14 | 204.5 | 86.5 | 14.8 | 15.9 | 23.3 | 0.0 | 168 | 172 | 11.8 |
| anim | 51 | 97.26 | 361.1 | 134.4 | 9.7 | 12.8 | 13.8 | 0.0 | 135 | 289 | 267.2 |
| idle | 0 | 0.00 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 148 | 0 | 0.0 |

The resident sizes at 2560x1600@2 were 47 MiB for X12 and 23 MiB for
compbench, with 14.6 MiB of client pools and 15.6 MiB of back buffer.

Observations:

- `blink` changes 40 logical pixels per frame and composes the whole
  window: 1.9 million device pixels and 14.8 ms per frame at scale 2. X12
  ignores the damage of the client.
- `pointer` spends 80 percent of its frame time in the flush. Each frame
  flushes two rectangles, and each flush makes two synchronous virtio
  requests.
- `idle` wakes X12 74 times a second without any change.
- The commit latency is 10 to 23 ms. Composition waits for the next
  expiration of the 16 ms timer.
- In `anim` at scale 2, the client spends 267 ms copying its private
  surface into the shared buffers, as much as X12 spends composing.

## G2: memcpy, memmove and memset (2026-10-06)

The string functions of libc copy 16 bytes per move at any alignment
(`libc.md`). The copy of 1 MiB under TCG rose by 32 to 35 percent at
every alignment. In the benchmark the flush, which copies the composed
rectangles into the framebuffer, fell from 69 to 52 ms for the drag and
from 141 to 100 ms for the pointer motion at 1280x800. The other values
remained within the variation between two runs, because the composition
copies only XRGB rows of clients with `memcpy`.

## G3: the pixel module and the rectangle set (2026-10-06)

`lib/libgui/src/pixel.c` with the header `gui/pixel.h` contains the row
operations of libgui and X12:

| Function | Operation |
|---|---|
| `pixel_fill` | a colour into n pixels |
| `pixel_copy_opaque` | the colour channels of a row, alpha byte 0 |
| `pixel_over` | a row with straight alpha over a row |
| `pixel_mask` | a colour with an 8 bit coverage row over a row |
| `pixel_darken` | every colour channel times keep / 255 |
| `pixel_walk_init`, `pixel_walk_next` | the nearest neighbour positions floor((start + i) * num / den) |
| `pixel_sample`, `pixel_sample_mask` | a row read at the positions of a walk, with a step of 1, -1 or a stride |
| `pixel_sample_over`, `pixel_mask_sample` | a sampled row blended over a row |
| `pixel_pack` | conversion into a framebuffer format that is not 0x00RRGGBB |

The inline functions `pixel_div255`, `pixel_blend` and `pixel_shade`
blend one pixel. Every blend divides by 255 with rounding, so alpha 0
leaves the destination and alpha 255 gives the source colour exactly.
A blended pixel retains the alpha byte of the destination. Before G3,
X12 and the font code divided by 256 with a coverage scaled to 256, the
painter and the wallpaper divided by 255 without rounding, and the
decorations used a float factor. The colours of blended pixels changed by
at most one per channel.

`pixel_over`, `pixel_mask` and `pixel_darken` have two forms with the
same result. The vector form places red and blue under the mask
0x00ff00ff and green and alpha shifted down by 8 bits into 16 bit
lanes, so that one 16 bit multiplication blends two channels without
unpacking, packing or shuffles. The word form places the four channels
of one pixel into four 16 bit fields of a 64 bit register. aarch64 uses
the vector form (NEON). Under QEMU TCG the SSE2 arithmetic of an x86_64
guest is emulated far more slowly than scalar code, while under KVM it
runs natively. On x86_64 the first call of a blending function therefore
measures both forms of each function on rows of 256 pixels, the fastest
of three runs of four rows, and retains the faster form. `pixel_forms()`
reports the choice. The owner chose this measurement on 2026-10-06 after
the first SSE2 version, which unpacked bytes into 16 bit lanes, ran at a
fifth of the speed of the scalar code under TCG.

Measured by `pixeltest` in megapixels per second over rows of 1024
pixels with partial alpha and coverage:

| | x86_64 under TCG | aarch64 under HVF |
|---|---|---|
| `pixel_over`: vector, word, reference | 58, 180, 187 | 1281, 728, 595 |
| `pixel_mask`: vector, word, reference | 83, 183, 196 | 1765, 930, 677 |
| `pixel_darken`: vector, word, reference | 467, 426, 152 | 3495, 1227, 2601 |
| chosen forms | over word, mask word, darken vector | all vector |

The references are the scalar loops of `src/pixel_impl.h`. Under TCG
the word form reaches the speed of the reference for `over` and `mask`
and three times that speed for `darken`. The vector form of `darken`
uses only 16 bit multiplications, additions, shifts and logical
operations, which QEMU translates into host vector instructions.

A loop that the linker places across a page boundary ran six times
slower under TCG than the same loop within one page. QEMU does not chain
the translated blocks of a loop across a page boundary and looks the
next block up on every pass. The hot functions of the pixel module and
of the libc string functions are therefore aligned with
`SIMD_WITHIN_PAGE` to a power of two above their size on both
architectures (`libc.md`).

`struct rect_set` in `gui/gfx.h` stores up to 32 disjoint rectangles.
`rect_set_add` merges a new rectangle with one of the set when their
bounding box adds at most a quarter of their area, and otherwise adds
the parts of it that the set does not cover yet. A set that would exceed
its size becomes the bounding box of everything. `rect_subtract` and
`rect_scale` are public. They replaced the private copies of X12
(`rect_subtract` in `scene.c`, `rect_minus` in `decor.c` and the three
copies of `dev`).

The module replaced the private loops in `scene.c` (the blend, the row
copies, the divisions per pixel of the scaled path and of the cursor),
`decor.c`, `backend_fb.c`, `paint.c`, `font.c`, `wallpaper.c`, `gfx.c`
and `csd.c`, and the private blends of `sysmon` and `paint`. The
compositing of the SVG codec is a different operation, with a
destination alpha, and remains.

Tests: `make check-libgui` runs `lib/libgui/tests/test_pixel.c` on the
host. It compares both forms and the chosen function with the reference
at every offset from 0 to 7 and every length up to 67, checks every
triple of alpha, source and destination channel, compares the walk with
the division and the rectangle set with a bitmap of the union. The case
`pixel` runs the same file in the guest through `pixeltest`, with a
sample of the triples, on x86_64 and on aarch64.

Benchmark after G3, compared with the baseline of G1:

| | baseline | after G3 |
|---|---|---|
| drag at 1280x800: compose ms, frame p50 ms | 149.6, 2.9 | 83.8, 2.0 |
| drag at 2560x1600@2: compose ms, frame p50 ms | 520.2, 9.7 | 313.3, 7.4 |
| resize at 2560x1600@2: compose ms | 296.2 | 205.4 |
| blink at 2560x1600@2: compose ms, frame p50 ms | 204.5, 14.8 | 152.9, 12.8 |
| anim at 2560x1600@2: frames of 60, frame p50 ms | 51, 9.7 | 60, 7.4 |

The flush times, the latencies and the wakeups did not change, because
G4 to G6 address them. blink still composes the whole window, because
X12 ignores the damage of the client until G5.

## G4 and G5: the flush and the present path (2026-10-06)

G4 flushes several rectangles in one request with two waits
(`display.md`). G5 composes only the damage of a client, adds no damage
for frame callbacks, composes on virtio-gpu directly into the
framebuffer and flushes every frame with one request (`compositor.md`).

At 2560x1600@2, compared with the baseline of G1:

| | baseline | after G5 |
|---|---|---|
| blink: composed Mpx, compose ms, frame p50 ms | 38.14, 204.5, 14.8 | 1.91, 5.3, 0.2 |
| drag: compose ms, flush ms, frame p50 ms | 520.2, 198.5, 9.7 | 287.5, 48.5, 5.4 |
| resize: compose ms, flush ms | 296.2, 130.5 | 167.5, 39.9 |
| anim: frames of 60, frame p50 ms | 51, 9.7 | 60, 2.9 |
| pointer: flush ms for 400 frames | 128.3 | 66.0 |

The blink composes 1.91 instead of 38.14 million device pixels. The
rest of it comes from libgui, which reports the union of the last two
frames as the damage of a commit (G8). The flush time fell to a quarter
for the drag, because the frame needs no copy into the framebuffer and
one request covers all its rectangles. The commit latency (10 ms) and
the wakeups did not change, because the 16 ms timer still paces the
frames until G6.

`comp_damage` checks the damage path exactly: ten buffers with one pixel
of damage each compose 40 device pixels at scale 2 in ten rectangles,
and ten commits with only a frame callback compose nothing. The cases
`comp_bench` and `comp_bench_hidpi` compare the CRC-32 of the guest
framebuffer with a screendump of the host, which shows that direct
composition flushes every pixel.

## G6: the timing model (2026-10-06)

The frame clock runs only while damage or a frame callback is pending,
and poll waits until the earliest deadline of the key repeat, the pings
and the input method (`compositor.md`). At 2560x1600@2, compared with
the baseline of G1:

| | baseline | after G6 |
|---|---|---|
| blink: commit latency ms, wakeups | 23.3, 168 | 0.6, 26 |
| pointer: input latency ms, wakeups | 8.2, 1166 | 0.7, 449 |
| resize: input latency ms, wakeups | 7.7, 996 | 0.3, 207 |
| idle for 2.3 s: wakeups | 148 | 6 |

A frame now starts at once when the previous one lies more than
`frame_ms` back. The drag and the animation still show a latency of 10
to 16 ms, because their events arrive faster than one per 16 ms and the
frames remain at least that far apart. `comp_idle` measures 17 wakeups in
five idle seconds with one window, which the pings of the window and the
connection of `compstat` cause. Before, the periodic timer woke X12
about 320 times in five seconds.

## G7: decorations without square roots per pixel (2026-10-06)

The antialiased shapes of the decorations come from tables computed once
per size: `pixel_corner_table` for rounded corners, `pixel_disc_table`
with `gfx_disc` for the round buttons, and in `csd.c` two profiles of
the outline and the shadow by the squared distance to the frame. Before
G7, X12 computed a square root for every pixel of a corner square and of
a button in every frame that touched them, and libgui computed two
square roots and float blends for every pixel of its chrome at each
resize and at each corner of every copy. X12 stores the title of each
toplevel shaped at the screen scale and draws it with
`painter_text_shaped`. `text_outline` shapes strings of up to 64
glyphs into an array on the stack instead of an allocation.

## G8: libgui without the private surface (2026-10-06)

A window draws straight into a slot of its shared memory pool
(`lib/libgui/src/buffers.c`, `gui.md`). Before G8, libgui painted into a
private surface and copied each damaged rectangle into one of two pool
buffers. A window therefore had three copies of its pixels. Now it has
two in normal operation. `gui_begin_paint` copies only the regions that
changed since the target slot was last current. The pool has a third
slot for the case that the compositor retains both buffers for 100 ms.
The third slot uses no memory until a window draws into it.

The kernel needed one change for this. A memfd page now gets its frame
at the first fault (`sockets.md`). Before G8, `ftruncate` allocated and
zeroed every page, and `mmap` mapped all of them. A three slot pool would
then have cost more memory than the private surface it replaces.

Measured with `comp_bench_hidpi` at 2560x1600@2. The compbench window
has a buffer of 1584x1204 device pixels, 7.3 MiB:

| | baseline (G1) | after G8 |
|---|---|---|
| compbench resident size | 23 MiB | 15.6 MiB |
| X12 resident size | 47 MiB | 31.5 MiB |
| client copy time, `anim` (60 frames) | 267.2 ms | 111.7 ms |
| client copy time, `blink` (20 frames) | 11.8 ms | 5.0 ms |

X12 maps the whole pool of each client, but only the pages of the used
slots become resident. The back buffer of X12 disappeared with the
direct composition of G5. In `blink` the client copies the window
contents once after the first frame and then 160 device pixels per
frame. In `anim` the client repaints the scene canvas in every frame and
copies the region of the previous frame first, because the framework
cannot know that a widget paints every pixel of its rectangle.

`gui_memory` checks the result: two used slots in the pool, a resident
size below the baseline minus one buffer with a margin of 1 MiB, and
unchanged contents, frame corners and shadow after 20 resize drags. A
narrow window painted its header buttons outside the header rectangle
and therefore outside the damage that it reported. `csd.c` now clips the
header to its rectangle. The host test of `buffers.c` found this defect.

## G9: the device cursor on virtio-gpu (2026-10-06)

On virtio-gpu the device shows the cursor above the framebuffer
(`display.md`, `compositor.md`). A pointer motion calls
`FBIO_CURSOR_MOVE` and composes nothing. Before G9 every motion damaged
the old and the new cursor rectangle, and a frame composed and flushed
both.

Measured with `comp_bench_hidpi` at 2560x1600@2. The `pointer` scenario
moves the pointer 400 times:

| | after G8 | after G9 |
|---|---|---|
| frames | 400 | 0 |
| composed device pixels | 790 400 | 0 |
| compose and flush time | 112.8 ms | 0 |
| time of the 400 cursor moves | | 14.5 ms |
| input latency, average and longest | 0.6 ms, 6.6 ms | 0.04 ms, 0.23 ms |

The input latency now ends with the cursor move instead of a frame
(`stats.c`). In `resize` the frames fell from 100 to 40, because the
motions between the corner drags no longer compose.

The tick based CPU time of X12 in `pointer` rose from 4 to 111 ms. The
value before G9 was too low: the frames alone took 112.8 ms. A frame
ran directly after the tick that expired the frame timer and ended
before the next tick, so the kernel rarely charged it. A cursor move
runs at the arrival of the input event, at any time within a tick.

QEMU draws the device cursor in its display window. Its screendump does
not contain the cursor, so `comp_cursor` checks the cursor that
`/dev/fb0` recorded.

## Summary of G1 to G9 (2026-10-06)

The baseline is the G1 run. The final values are the range of two runs
of `comp_bench` and `comp_bench_hidpi` on the code after G9. The cases
run in parallel QEMU instances under TCG, so times vary by up to a
factor of two between runs. Counters such as frames, composed pixels
and wakeups do not vary.

2560x1600 at scale 2:

| scenario and value | baseline | after G9 |
|---|---|---|
| drag: compose ms, flush ms | 520.2, 198.5 | 264 to 266, 36 to 38 |
| drag: frame p50 ms, p95 ms | 9.7, 13.8 | 4.9, 5.4 |
| pointer: frames, compose and flush ms | 400, 183.7 | 0, 0 |
| pointer: input latency ms, wakeups | 8.2, 1166 | 0.04 to 0.07, 430 |
| resize: frames, compose ms, flush ms | 100, 296.2, 130.5 | 40, 188 to 227, 42 to 51 |
| resize: commit latency ms, wakeups | 17.2, 996 | 7.9, 199 to 205 |
| blink: composed device pixels | 38.1 million | 3200 |
| blink: compose ms, flush ms, frame p50 ms | 204.5, 86.5, 14.8 | 1.5 to 2.0, 5.3 to 6.0, 0.3 to 0.4 |
| blink: commit latency ms, wakeups | 23.3, 168 | 0.4 to 0.5, 26 |
| anim: frames of 60, frame p50 ms | 51, 9.7 | 59, 3.2 to 3.5 |
| anim: compose ms, flush ms | 361.1, 134.4 | 152 to 185, 36 to 40 |
| anim: client copy ms | 267.2 | 81 to 110 |
| idle: wakeups in 2.3 s, in 5 s with one window | 148, about 320 | 6, 17 to 19 |
| resident size: X12, compbench | 47 MiB, 23 MiB | 31.6 MiB, 15.6 MiB |

1280x800 at scale 1:

| scenario and value | baseline | after G9 |
|---|---|---|
| drag: compose ms, flush ms, frame p50 ms | 149.6, 69.3, 2.9 | 75 to 76, 15 to 19, 1.3 to 1.5 |
| pointer: frames, compose and flush ms, input latency ms | 400, 172.7, 8.4 | 0, 0, below 0.1 |
| resize: frames, compose ms, flush ms | 100, 86.8, 46.4 | 40, 61 to 82, 18 to 20 |
| blink: compose ms, flush ms, commit latency ms | 52.8, 22.3, 13.6 | 1.5 to 1.7, 5.3 to 5.8, 0.4 to 0.5 |
| anim: compose ms, flush ms, client copy ms | 130.4, 50.6, 86.8 | 45 to 101, 17 to 23, 25 to 44 |

Values that did not improve:

- The commit latency of `anim` remains 13.6 ms. The client commits every
  16 ms, and the frame clock starts a frame at most every `frame_ms`
  (16 ms) after the previous one. A commit therefore waits for the next
  frame time.
- The client copy of `blink` lies between 5.0 and 13.9 ms, against 11.8
  ms before. Almost all of it is one copy of the window contents, when
  the window paints into its second slot for the first time. The later
  frames copy 160 device pixels each.
- Since G9 the input latency of `drag` ends at the move of the device
  cursor. The window follows the pointer with the frames, which the
  drag rows show.
- The tick based CPU time of X12 is not comparable between the runs
  (see G9).

## Idle session (2026-10-06)

An idle session at 2560x1600@2 with panel, desktop, a terminal and the
Performance page of x12settings composed 5.8 million device pixels per
second. A temporary log of every commit showed the sources:

- The desktop read its directory every second and repainted its whole
  surface each time, also where windows covered it. It now repaints only
  when the entries changed.
- The panel redrew itself every second, although its clock shows
  minutes. It now redraws when the clock text or a setting changed.
- X12 composed the damage of a surface also where opaque surfaces above
  it covered the surface. `scene_damage_surface` now leaves those parts
  out. It uses the same opacity rules as the composition.
- The framework sent the bounding box of all repainted widgets. It now
  sends a rectangle per repainted widget, which `rect_set` merges when
  they lie close together.

Afterwards the same session composed 1.7 million device pixels per
second. All of them come from the Performance page, whose four graphs
and table change every second. On another page of x12settings the
session composes nothing.

## K2: widget counters, baseline (2026-10-09)

The counters of K2 before the changes of K2. Each row adds up the steps
of one scenario. A paint is one paint pass of the window that changed
pixels.

Screen 1280x800 at scale 1:

| scenario | paints | paint ms | widget paints | painted Mpx | layouts | text shapes |
|---|---|---|---|---|---|---|
| wheel | 30 | 55.3 | 30 | 11.08 | 0 | 1260 |
| type | 20 | 6.9 | 20 | 0.40 | 0 | 80 |
| status | 20 | 25.6 | 120 | 8.21 | 240 | 80 |
| edit | 20 | 454.7 | 20 | 8.21 | 0 | 8080 |
| scroll | 20 | 54.9 | 4100 | 8.21 | 8220 | 4445 |
| hover | 0 | 0.0 | 0 | 0.00 | 0 | 0 |

Screen 2560x1600 at scale 2:

| scenario | paints | paint ms | widget paints | painted Mpx | layouts | text shapes |
|---|---|---|---|---|---|---|
| wheel | 30 | 129.3 | 30 | 44.32 | 0 | 1260 |
| type | 20 | 13.0 | 20 | 1.58 | 0 | 80 |
| status | 20 | 107.3 | 120 | 32.83 | 240 | 80 |
| edit | 20 | 530.3 | 20 | 32.83 | 0 | 8080 |
| scroll | 20 | 166.4 | 4100 | 32.83 | 8220 | 4445 |
| hover | 0 | 0.0 | 0 | 0.00 | 0 | 0 |

The table shows the following defects:

- All 30 wheel steps repaint the table. The 10 steps up at the top and
  the last 6 steps down change no pixel.
- A status label update measures and lays out the whole window and
  repaints all of it: 6 widget paints, 12 layouts and the whole window
  as damage per step.
- A wheel step in the scroll area lays out the area and its 200 labels
  and repaints all of them: 205 widget paints and 411 layouts per step.
- Each keystroke in the editor repaints the whole editor and shapes 404
  runs of text. The editor rebuilds its rows and runs the highlighter
  from the first line (K3).
- The hover scenario paints nothing, because rows have no hover state
  yet (K5).
