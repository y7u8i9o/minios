# Compositor performance

This plan reduces the CPU time, the latency and the memory of the
graphical session and measures each change. It covers the compositor X12
(`user/compositor/`), the client side of libgui, the graphics drivers of
the kernel and the string functions of libc. Each milestone below ends
with boot tests in `tests/cases/` and is marked completed here, with the
numbers before and after it, when they pass.

## 1. Scope

The owner chose the following scope on 2026-10-06.

- Goals: measurement, frame time and CPU, latency, memory.
- Layers: X12, the client side of libgui, the kernel graphics drivers
  (virtio-gpu and fbdev, including the virtio cursor queue), and
  `memcpy`, `memmove` and `memset` of libc.
- The inner pixel loops use SSE2 on x86_64 and NEON on aarch64. Each
  vector loop has a scalar reference and a test that compares the two.
- No GPU 3D, no blur and no new visual effects.

The starting point on 2026-10-06:

- A GUI window at 2560x1600@2 occupies three copies of its pixels: the
  private libgui surface and two pool buffers. The system monitor has
  about 22 MiB of them.
- X12 ignores the damage of a client. A commit with a new buffer
  recomposes and flushes the whole window, for example at every blink of
  the terminal cursor.
- Each damaged rectangle costs two synchronous virtio-gpu round trips.
- On virtio-gpu, X12 copies every frame from its back buffer into the
  framebuffer.
- A periodic 16 ms timer wakes X12 also when it is idle. Composition
  waits for the timer, which adds 0 to 16 ms of latency.
- The decorations compute `sqrtf` and float blends for each pixel. The
  scaled paths divide for each pixel.
- `memcpy` copies byte by byte when the two pointers differ in alignment
  by 4 bytes.
- The frame statistics have a resolution of one millisecond.

## 2. Fixed decisions

- Times are microseconds of `CLOCK_MONOTONIC`, read through the new libc
  function `uptime_us()`. Tests assert only deterministic counters:
  composed device pixels, flush rectangles, flushed bytes, pool bytes
  and wakeups. Times are printed. A time receives a bound only after its
  baseline is known.
- The `debug` interface of `protocol/debug.xml` moves to version 2 with
  the requests `get_frame_stats` and `reset_frame_stats` and the event
  `frame_stats`. The command `compstat` prints the statistics and
  `compstat -r` resets them.
- The pixel loops of X12 and libgui are in one module,
  `lib/libgui/src/pixel.c` with the header `gui/pixel.h`. The functions
  work on rows. The public header contains no vector types. The scalar
  references are in `src/pixel_impl.h`.
- Vector code uses GCC vector extensions through integer types in
  `minios/simd.h`. The architecture is selected at compile time.
- Every blend divides by 255 with rounding:
  `(t + 128 + ((t + 128) >> 8)) >> 8`. Alpha 0 and alpha 255 give exact
  results.
- `struct rect_set` in `gui/gfx.h` stores up to 32 disjoint rectangles.
  Two rectangles merge only when their bounding box adds at most a
  quarter of their area. A full set collapses to its bounding box.
  `rect_subtract` and `rect_scale` are public in `gfx.c` and replace the
  private copies in the compositor and in libgui.
- `FBIO_FLUSH_RECTS` flushes up to 32 rectangles in one call. Only the
  display owner may call it while an owner exists. `FBIO_FLUSH` does not
  change.
- virtio-gpu submits all transfers of a flush and waits once, then
  submits all flushes and waits once. The batch runs under the existing
  `g->lock`.
- X12 composes directly into the framebuffer mapping when the device
  needs a flush (`FB_CAP_FLUSH`), the format is native and the pitch is
  a multiple of 4. Otherwise it composes into its back buffer as before.
- A new buffer with unchanged geometry damages only the rectangles that
  the client sent. A frame callback adds no damage. The opaque region of
  a surface decides its opacity, across all of its rectangles.
- X12 composes when damage exists and `frame_ms` have passed since the
  last presentation. Otherwise it arms a one-shot timer. The poll timeout
  is the earliest deadline of the seat, hang and input method modules.
  X12 flushes once per frame.
- Antialiased decoration shapes come from coverage tables, computed once
  per radius and scale.
- A libgui window has two pool buffers and no private surface.
  `gui_begin_paint` prepares the target buffer by buffer age. The libgui
  ABI becomes 2.
- On virtio-gpu the pointer is a hardware cursor of the virtio cursor
  queue. Screen capture composes the cursor in software.

## 3. Milestones

### G1. Frame statistics and the benchmark cases (completed 2026-10-06)

libc gains `uptime_us()`. The new file `user/compositor/stats.c` counts
frames, rectangles, composed device pixels, flush calls, flushed bytes,
loop wakeups and empty timer expirations. It measures the compose and the
flush time (sum, maximum and a histogram for p50, p95 and p99), the
latency from the first damage to the end of the flush, from a commit to
its presentation and from pointer input to its presentation, and the CPU
time of X12. It reports the bytes of the back buffer and of the mapped
client pools. The counters replace the millisecond statistics of
`scene.c`. The `debug` interface version 2 delivers them, `compstat`
prints them and `x12settings` shows them.

libgui gains `gui_get_stats()`: paints, paint time, commits, copy time,
copied bytes, frame waits and pool bytes. The benchmark client
`compbench` opens a framework window of 760x540 and runs the scenarios
`blink`, `anim`, `idle` and `resize`. The kernel test runs each scenario
between `compstat -r` and `compstat`, adds a window drag and pointer
motion driven from the kernel, and prints the CPU time and RSS of X12
and of the client.

Tests: `comp_bench` (virtio-gpu, 1280x800) and `comp_bench_hidpi`
(virtio-gpu, 2560x1600@2) require one line per scenario with a frame
count consistent with the scenario. `make check-libgui` covers the stub
of the test client.

Document: `docs/design/graphics-performance.md`,
`docs/design/compositor.md`, `docs/design/protocol.md`.

The implementation differs from the text above in three points. The
`debug` interface sends one `frame_stat` event per key with the high
and the low half of the value, then `frame_stats_done`, so a new value
needs no change of the protocol. X12 applies a resize at the release of
the button and rejects a buffer size that no configure acknowledged, so
the resize scenario is 20 corner drags driven by the boot test instead
of a client scenario. The benchmark runs X12 without the panel, so that
the clock of the panel does not add frames.

The baseline is in `docs/design/graphics-performance.md`. At 2560x1600@2
a blink of 2x20 pixels composes 1.9 million device pixels and takes 14.8
ms, the idle X12 wakes 74 times a second, and the commit latency is 14
to 23 ms. The CPU time that the kernel charges per tick is too low for
frames shorter than a tick, so the tables give it for reference only.

`gui_bind_global` now treats a global with a lower advertised version
as missing, because a bind above that version ends the connection. The
host builds of libgui and Lua declare `uptime_ms` and `uptime_us` in
`tests/host_compat.h`, which replaced three local declarations and one
private copy.

### G2. memcpy, memmove and memset (completed 2026-10-06)

`memcpy` aligns the destination and copies 16 bytes per load and store
at any relative alignment. `memmove` copies backwards in the same way
when the regions overlap with the destination above the source.
`memset` stores 16 bytes at a time. An attribute prevents GCC from
turning the loops into calls of the functions themselves.

Tests: `libctest` checks offsets 0 to 31, lengths 0 to 300 and 65 537,
overlapping `memmove` in both directions and `memset`, and prints the
throughput. The case `libc` runs on x86_64 and aarch64.

Document: `docs/design/libc.md`, `docs/design/graphics-performance.md`.

The copy of 1 MiB under TCG on x86_64 rose from 6469 to 8563 MB/s with
aligned addresses, from 5020 to 6759 MB/s at an offset of 4 bytes and
from 4562 to 6030 MB/s at an offset of 1 byte. The fill rose from 10126
to 13157 MB/s. On aarch64 with HVF the fill rose from 51 to 98 GB/s. In
the benchmark the flush time at 1280x800 fell from 69 to 52 ms for the
drag and from 141 to 100 ms for the pointer motion. The other values
remained within the variation between two runs.

### G3. The pixel module and the rectangle set (completed 2026-10-06)

`minios/simd.h` gains integer vector types. `pixel.c` provides
`pixel_fill`, `pixel_copy`, `pixel_copy_opaque`, `pixel_over`,
`pixel_mask`, `pixel_darken`, `pixel_scale_int`, `pixel_sample` and
`pixel_pack` with SSE2 and NEON paths. `gfx.c` provides `rect_set`,
`rect_subtract` and `rect_scale`. The module replaces the private loops
of `scene.c`, `decor.c`, `backend_fb.c`, `paint.c`, `font.c`,
`wallpaper.c`, `gfx.c` and `csd.c`, including the divisions per pixel
of the scaled paths.

Tests: `make check-libgui` compares each vector function with its
reference at every alignment and for row lengths 0 to 67, checks every
pair of alpha and channel value, and compares `rect_set` with a bitmap
of the union. The case `pixel` runs the same comparison in the guest on
x86_64 and aarch64 and prints the throughput. `comp_scale`,
`wallpaper_hidpi`, `comp_panel` and the `gui_*` cases show no change.

Document: `docs/design/graphics-performance.md`, `docs/design/gui.md`,
`docs/design/images.md`.

The first SSE2 version unpacked the bytes of a pixel into 16 bit lanes
and ran at a fifth of the speed of the scalar code under TCG, because
QEMU emulates the unpack, pack and shuffle instructions. The blend now
masks two channels into 16 bit lanes and needs no such instructions. It
still lost against scalar code under TCG. The owner chose on 2026-10-06
that x86_64 measures the vector form and a 64 bit scalar word form at
the first call and retains the faster one, which replaces the fixed
decision of a choice at compile time. aarch64 uses NEON. Under TCG the
choice is the word form for `pixel_over` and `pixel_mask` and the vector
form for `pixel_darken`. The vector types need no inline assembly,
because GCC and clang accept `__builtin_shufflevector` and
`__builtin_convertvector`.

A loop across a page boundary ran six times slower under TCG, because
QEMU does not chain translated blocks across pages. The hot functions of
the pixel module and of the string functions of libc are aligned with
`SIMD_WITHIN_PAGE` so that none crosses a page.

`pixel_copy` and `pixel_scale_int` were not needed: `memcpy` copies rows,
and the walk gives exact positions for integer factors. The module
gained `pixel_sample_over` and `pixel_mask_sample` for sampled blends
and `pixel_forms` for the report of the choice. The private blends of
`sysmon`, `paint` and the button discs of `csd.c` also use the module.
At 2560x1600@2 the drag composes in 313 instead of 520 ms and the median
frame of the animation fell from 9.7 to 7.4 ms
(`docs/design/graphics-performance.md`).

### G4. Several rectangles per flush (completed 2026-10-06)

`FBIO_FLUSH_RECTS` with the capability `FB_CAP_FLUSH_RECTS`.
`fb_flush_rects` in fbdev. virtio-gpu splits its control transfer into
submission and wait and notifies the device once per batch. The console
flush thread uses the same path.

Tests: in `gpu_mode`, `fbmodetest` draws three patterns and flushes two
of them in one call. A screendump checks that the host shows the two and
not the third. A process that is not the owner receives `EPERM`, a count
of 33 receives `EINVAL`. The case runs on x86_64 and aarch64.

Document: `docs/design/display.md`, `docs/design/locking.md`.

On aarch64 the screendump showed the square that was not flushed. The
console marked the whole screen dirty when it scrolled before the
acquisition, and `gpu_flushd` flushed that rectangle up to 20 ms later
with the pixels of the owner. Under hardware acceleration the owner had
drawn its squares by then. The acquisition now discards a pending
rectangle, and the console reports none while a process owns the
display. With direct composition in G5 such a flush would have shown
half composed frames. The post script of `gpu_resize` uses the shared
`tests/ppm_pixels.py` instead of its own reader.

### G5. The present path of the compositor (completed 2026-10-06)

X12 honours the damage of a client, and a frame callback adds no damage.
The scene stores its damage in a `rect_set`, composes every rectangle and
flushes once. `draw_surface` copies the opaque spans of all opaque
rectangles and blends the others. The built-in arrow becomes an ARGB
image per scale. On virtio-gpu X12 composes into the framebuffer mapping.

Tests: `comp_damage` commits 10 buffers with 1x1 damage and requires
exactly 10·S² composed device pixels and 10 flush rectangles. Ten
commits with only a frame callback compose nothing and receive 10
`done` events. `comp_bench_hidpi` compares a region of the guest
framebuffer with the host screendump. `comp_core`, `comp_scale`,
`gui_askpass`, `gui_*` and `panel_*` show no change.

Document: `docs/design/compositor.md`, `docs/design/display.md`.

A frame callback without damage receives `done` at the next tick of the
16 ms timer, without a composition. A new scale, transform or opaque
region of a surface damages it whole. The screendump comparison runs in
`comp_bench` as well as in `comp_bench_hidpi`, after the drag, and
`comp_damage` runs at 2560x1600@2 on virtio-gpu, so that it covers the
direct composition. The compositor rule of `tests/map` adds `comp_damage`
and `gui` on aarch64. At 2560x1600@2 the blink composes 1.91 instead of
38.14 million device pixels, and the drag flushes in 48.5 instead of
198.5 ms (`docs/design/graphics-performance.md`).

### G6. The timing model (completed 2026-10-06)

The periodic timer is replaced by a one-shot timer that X12 arms only
when work is pending. The seat, hang and input method modules report
their next deadlines, and the poll timeout follows from them.

Tests: `comp_idle` requires 0 frames and fewer wakeups than the G1
baseline in five idle seconds. `comp_bench` prints the latency from a
commit to its presentation. `gui_repeat`, `gui_text_repeat`, `ime*`,
`comp_hang` and `gui_drag` show no change.

Document: `docs/design/compositor.md`.

The first version scheduled a frame only after poll returned. At the
start, without a client or deadline, poll waited indefinitely and the
first frame never appeared, which `gui_pointer` and `gui_scale2`
showed. `schedule_frame` now runs at the start of every pass of the
loop. The bound of `comp_idle` is 40 wakeups in five seconds, measured
17. `tests/map` runs `comp_idle` on aarch64 for a change of `main.c`. At
2560x1600@2 the commit latency of the blink fell from 23.3 to 0.6 ms and
the input latency of the pointer from 8.2 to 0.7 ms
(`docs/design/graphics-performance.md`).

### G7. Decorations without square roots per pixel (completed 2026-10-06)

`pixel.c` gains coverage tables for rounded corners and discs. The
server decorations of X12 and the client decorations of libgui use them,
and X12 stores the shaped title of each toplevel. `text_outline` shapes
into an array on the stack.

Tests: `make check-libgui` compares the chrome from the tables with the
present float code within 1 per channel at scales 1 to 3. The title bar
checks of `gui`, `gui_wm` and `comp_shell` show no change.

Document: `docs/design/compositor.md`, `docs/design/gui.md`.

### G8. libgui without the private surface (completed 2026-10-06)

A window has two pool buffers and paints into the target buffer.
`gui_begin_paint` waits for its release, copies the stale rectangles of
the presented buffer and restores the raw corners of the client
decoration. A commit sends the damage of its frame. The framework paints
a window only while no frame is pending. Pool slots have a quarter of
headroom, and an old pool is destroyed only after all its buffers are
released. This also fixes the destruction of the buffer on screen by
two resizes before a commit.

Tests: a host test simulates paint, commit, release, resize and scale
change for opaque, decorated, translucent and popup windows and compares
the presented buffer with a reference picture. `gui_memory` at
2560x1600@2 requires two buffers in the pool, a lower RSS than the G1
baseline, and correct pixels after 20 resize steps.

Document: `docs/design/gui.md`, `docs/design/framework.md`,
`docs/design/packages.md`.

Result: a memfd page now gets its frame at the first fault. Before this
change the kernel allocated every page at `ftruncate`, and the third
slot and the headroom would have cost memory. At 2560x1600@2 the
resident size of compbench fell from 23 to 15.6 MiB and that of X12
from 47 to 31.5 MiB. The client copy time of `anim` fell from 267.2 to
111.7 ms. `tools/mkbase.py` now packs every base package again when an
ABI number changes. Before, the unchanged packages retained the old
`needs` lines and the image installation refused them
(`docs/design/graphics-performance.md`).

### G9. The hardware cursor on virtio-gpu

virtio-gpu drives the cursor queue. `FBIO_CURSOR_SET` and
`FBIO_CURSOR_MOVE` with the capability `FB_CAP_CURSOR` set the image and
the position. X12 moves the hardware cursor without damage and composes
the cursor in software only for screen capture and on devices without the
capability.

Tests: `comp_cursor` requires 400 pointer motions to compose and flush
nothing, and a screendump shows the cursor at its position. The cursor
cases on std VGA show no change.

Document: `docs/design/display.md`, `docs/design/compositor.md`.

## 4. Size

The work changes about 9500 lines, tests and documents included: G1
1500, G2 400, G3 2200, G4 550, G5 1100, G6 450, G7 850, G8 1700 and G9
700.
