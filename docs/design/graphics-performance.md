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
| `flushes` | calls of `backend_flush` |
| `flush_bytes` | bytes that the flushes copied into the framebuffer |
| `compose_us`, `compose_max_us` | time of the composition, total and the longest |
| `flush_us`, `flush_max_us` | time of the flushes, total and the longest |
| `frame_max_us` | the longest frame |
| `frame_p50_us`, `frame_p95_us`, `frame_p99_us` | percentiles of the frame time |
| `damage_latency_us`, `_max_us` | from the first damage after a frame to the end of the next frame |
| `commit_latency_us`, `_max_us` | from the earliest commit of a buffer to the end of the frame that shows it |
| `input_latency_us`, `_max_us` | from the earliest pointer motion to the end of the frame that shows it |
| `wakeups` | returns of `poll` in the main loop |
| `idle_timers` | expirations of the frame timer without damage |
| `cpu_us` | CPU time of X12, tick based (see above) |
| `back_bytes` | bytes of the back buffer |
| `pool_bytes` | bytes of the mapped client pools |

The flush time is the time spent in `backend_flush`: the copy into the
framebuffer and the `FBIO_FLUSH` ioctl. The compose time is the rest of
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
them. x12settings shows the frame time, the flush time, the latencies
and the pixel memory on its Status tab. X12 logs `slow frame: N us` for
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
