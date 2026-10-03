# Lua Synthesizer playback investigation — 2026-09-10

The reproduced bottleneck is deadline contention in the application, amplified
by high-resolution GUI work. Sustained mixing/copy throughput in audiod was not
the limiting factor in these comparisons. This does not establish that every
scheduler, device, or host audio backend path is free of timing problems.

## Native audio worker results

Version 0.1.1 runs two native pthreads with independent Lua states. The GUI
thread sends controls through a bounded queue. The audio worker owns the
engine and playback connection, polls independently, and sends replaceable
meter snapshots without waiting for the GUI. No scheduler, audiod, driver or
C synthesizer change was needed for this separation. Libc thread-entry stack
alignment and stream-registry synchronization were corrected while testing
native workers.

The latest controlled run used private images, QEMU TCG with two virtual
CPUs, virtio graphics at 2560x1600 with scale 2, the WAV backend, and the
default Bright keys preset. Every phase measured ten seconds after a 500 ms
warmup. The inline reference uses the current optimized engine and GUI,
with audio refill restored to the main event loop for comparison.

| Mode | Voices | Mean render / 10 ms buffer | Maximum render | Stream underruns / phase | Audio thread CPU | GUI thread CPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Inline reference | 8 | 10.105 ms | 51.001 ms | 133 | 6564 ms combined | Same thread |
| Separate worker | 0 | 2.876 ms | 10.162 ms | 0 | 2041 ms | 177 ms |
| Separate worker | 1 | 3.341 ms | 8.981 ms | 0 | 2255 ms | 253 ms |
| Separate worker | 8 | 7.985 ms | 47.262 ms | 4 | 6176 ms | 364 ms |
| Separate worker, GUI paused once | 8 | 7.568 ms | 49.062 ms | 10 | 5723 ms | 340 ms |

During the deliberate 500 ms GUI sleep, the worker advanced 24480 frames
(510 ms of PCM) with zero additional stream underruns. Frame counts include
buffer quantization and the command barriers immediately around the sleep.
The paused phase's ten underruns occurred outside that interval. This proves
that audio progresses without GUI event processing. It does not establish
that GUI and DSP never compete for CPUs or the shared libc allocator.

Eight-voice underruns fell from 133 to 4 in this comparison, but remain
nonzero. Mean render time is already about 8 ms for a 10 ms audio quantum,
and occasional elapsed render calls reach roughly 47 ms. Further work
should reduce DSP cost and investigate those remaining long delays. These
sequential observations are sensitive to host load and are not a precise
causal speedup ratio or a guarantee of glitch-free playback.

CPU values now come from `sys.usage('thread')`, backed by kernel getrusage
accounting. Render durations use `sys.clock_ns()`, so they include preemption
and waiting. CPU accounting is tick-granular. Maximum render includes warmup,
while the mean and underrun counts are phase deltas. Inline audio and GUI CPU
are the same thread and must not be added together.

The live keyboard test and native-worker lifecycle test also passed. Closing
the instrument joins its worker, and every profile phase checks that no worker
remains. Raw latest evidence is in
`build/lua-threads-validation/performance/retina/serial.txt` and the targeted
regression logs under `build/lua-threads-validation/final-regression/`.

The final targeted matrix passed seven of eight cases. The existing
`gui_lua` case hit an intermittent kernel scheduler panic during desktop
teardown, then passed an isolated rerun. An earlier full targeted matrix
passed eight of eight. The panic (`enqueue waiter x12`) remains unresolved
in unchanged scheduler code and is separate from the corrected thread-entry
stack fault. Details and both outcomes are retained in
`build/lua-threads-validation/REPORT.md`.

## Earlier single-thread investigation

The following measurements predate the native worker. They explain the
original source of underruns and the engine/GUI optimizations retained in
the current application.

## Counters and deadlines

The instrument's Underruns value is the playback stream's xrun count.
`user/audiod/main.c:mix_playback` increments it when an active, previously
started stream has no queued buffer to mix. Device underruns have separate
counters/logs. Thus an increase on the instrument means audiod lacked that
client's audio, not necessarily that the host sound device ran dry.

The stream has three 480-frame buffers at 48 kHz: 10 ms each and at most 30 ms
queued. audiod's device queue is a separate four-period ring. Before version 0.1.1, audio dispatch, Lua rendering, buffer submission, GUI
events, layout and painting all executed on the instrument's single
event-loop thread. An event-loop pause can exhaust
the stream's queue even when average CPU use is below one full core.

## Reproduction and isolation

Serial QEMU TCG runs used two virtual CPUs, virtio graphics, the WAV audio
backend, and private kernel/initrd/disk snapshots. The preset was Bright keys.
The diagnostic compared prerecorded PCM, live rendering without a window,
the full window, and selectively disabled GUI operations. Each short phase
measured four seconds after warming the waveform cache and delay buffers.

At 2560x1600 with scale 2, the initial matrix recorded:

| Workload | Mean render time per 10 ms buffer | Stream underruns / 4 s |
| --- | ---: | ---: |
| Precomputed PCM, no window | 0.008 ms | 0 |
| Live DSP, one voice, no window | 3.94 ms | 0 |
| Live DSP, eight voices, no window | 7.66 ms | 0 |
| Full GUI, eight voices | 8.63 ms | 16 |
| GUI timer disabled, eight voices | 7.57 ms | 0 |

For the headless eight-voice phase, the kernel accounted approximately 2392 ms
of CPU to Lua and 58 ms to audiod during 4002 ms elapsed. Buffer submission
occupied 13 ms elapsed across 401 buffers. These results point to DSP and
application scheduling margins, rather than sample copying or mixer throughput.
CPU accounting is sampled at timer ticks, so short callbacks can be undercounted.

The same matrix at 1024x768 produced no underruns in the ordinary eight-voice
GUI phase, although its timer-disabled phase had two. This variability is a
reason to retain deadline distributions and longer runs, rather than treating
one short zero as proof of uninterrupted playback.

A second high-resolution run isolated GUI work with eight voices:

| Variant | Underruns / 4 s | Maximum interval between refill callbacks |
| --- | ---: | ---: |
| Original GUI | 21 | 53 ms |
| Freeze only status text | 1 | 23 ms |
| Disable scope painting | 15 | 51 ms |
| Stop automatic Lua garbage collection | 12 | 45 ms |
| Disable periodic GUI timer | 0 | 21 ms |

`widget_set_text` calls `widget_relayout`. The status included a changing peak
value, refreshed every 100 ms. This repeatedly forced full-window layout and
painting. `window_paint` explicitly marks the whole window dirty after layout.
The resulting application/compositor work competed with the audio deadline.
Stopping GC did not eliminate the problem, so GC alone is not a sufficient
explanation. The no-GC variant is a short diagnostic, not a recommended mode.

## Changes and limits

The status is now a fixed canvas. Updating it invalidates only that widget,
without changing layout. Keyboard painting happens on note/octave transitions
instead of on every display timer tick. The sound engine reuses its work arrays
and packs 64 frames per string instead of one frame per string: an ordinary
480-frame quantum produces eight intermediate strings instead of 480.
No audio-server, driver, scheduler, or C synthesizer behavior was changed.

The status-only change was insufficient: a subsequent ten-second eight-voice
phase still recorded 190 underruns, with render calls extending to 41 ms.
After also changing packing, a ten-second-per-phase high-resolution run gave:

| Voices | Mean render time / 10 ms | Render p99 | Maximum callback gap | Underruns / 10 s |
| --- | ---: | ---: | ---: | ---: |
| 0 | 2.69 ms | 3.05 ms | 23 ms | 0 |
| 1 | 3.19 ms | 3.85 ms | 33 ms | 0 |
| 8 | 8.56 ms | 20.02 ms | 47 ms | 11 |

These are sequential observations, not a precise causal speedup ratio: host
load, scheduling and cache/GC state vary. The remaining eight-voice misses mean
this was an improvement with remaining deadline misses. The native worker
measured above now runs generation independently of the GUI event loop.
Further DSP cost reduction remains useful. More buffering would tolerate
longer pauses at the cost of playing latency, without fixing a producer
that falls behind indefinitely.

The WAV backend does not validate Core Audio timing or speaker playback.
Cold waveform generation, dragging controls and running other desktop apps
can impose additional delays beyond this steady-state matrix.

## Validation and reproduction

`make check-lua` passes, including a regression asserting that a status update
does not repaint the keyboard. The packing change produced byte-identical
output across 660 blocks spanning all presets, varying block sizes, eight
voices, release tails and panic. The live three/eight-key GUI test passed and
its WAV post-check verified 139500 audible and 129928 distinct stereo frames.
The diagnostic passing means it completed. It intentionally has no
machine-dependent zero-underrun assertion. The original C synth files retain their
pre-change SHA-256 values.

The current `tests/cases/luasynth_profile` runs the inline/worker matrix
shown at the top of this document, with four seconds per phase by default.
Kernel `profile_mode=stress` selects ten-second phases. The former `detail`
isolation mode has been replaced by the two-thread comparison. In an
already running desktop with the installed 0.1.1 package:

    lua /etc/tests/luasynth-profile.lua /usr/share/apps/luasynth/ stress

The historical logs are under `build/audio-profile/display`, `detail`, `fixed`, and
`optimized`. `build/audio-profile/results.json` retains the numeric results.
The earlier `baseline` directory used provisional CPU labels and should not
be used as a CPU-utilization source.

Timing correction: MiniOS `clock()` (and Lua `os.clock()`) currently returns
monotonic elapsed time, despite the standard CPU-time meaning. Earlier synth
benchmark output incorrectly called that CPU time. The benchmark label is now
clock time. Historical diagnostic CPU values came from kernel `/dev/proc`
counters, while the current diagnostic uses per-thread `sys.usage`. `outside_pump_wall_ms` includes waiting and GUI
work and must not be interpreted as GUI CPU time.

The application archive is now `luasynth-0.1.1.mpk`. Close the instrument
and use `pkg install /usr/share/packages/luasynth-0.1.1.mpk` to upgrade an
existing 0.1.0 installation on a rebuilt system. The updated Lua interpreter
is also required. Saved patches outside package-owned files are preserved.
