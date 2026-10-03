# Milestones M27 to M34: floating point, calculator, audio, virtio-gpu and HiDPI

## M27. User floating point runtime and SSE2 vectors (completed 2026-08-30)

1. User compilation explicitly targets SSE2 with scalar floating point in
   XMM registers and enables GCC loop/SLP vectorization with its dynamic cost
   model. AVX remains disabled because M23's FXSAVE context contains XMM but
   not the upper halves of YMM registers. Kernel compilation retains every
   floating point and SIMD prohibition.
2. `libc/include/math.h` and `libc/src/stdlib/math.c`: IEEE-754 classification,
   constants, `fabs`, `copysign`, `sqrt`, integral rounding, `fmin`, `fmax`,
   `frexp`, `ldexp`/`scalbn`, and `modf`, with float variants and `EDOM` or
   `ERANGE` reporting where applicable.
3. `strtod` and `atof`: decimal fraction and exponent parsing, infinity, NaN,
   end-pointer handling, and range errors. The printf family gains `%f`, `%e`
   and `%g`, uppercase forms, and their width, precision, sign, alignment,
   zero-padding and alternate-form behavior.
4. `libc/include/minios/simd.h` exposes 128-bit `f32x4`, `f64x2` and `i32x4`
   types with construction, unaligned load/store, arithmetic and reductions.
   libc array routines provide float/double addition and dot products plus
   float SAXPY, using packed SSE2 loops followed by scalar tails.
5. `docs/design/floating.md` records the hardware-state contract, libc scope,
   SIMD API and the XSAVE/XRSTOR work required before AVX can be enabled.

Tests: `tests/cases/float` (`/bin/floattest`: scalar math, NaN and infinity,
subnormal and overflow, decimal parsing, floating printf conversions,
unaligned vector access, tails, SAXPY and reductions), the existing `fpu`
context-switch/fork/signal test, disassembly checks for packed SSE2
instructions, host framework checks, and the full boot suite (65 passed).

## M28. Extended libm, hexadecimal conversion and SSE2 primitives (completed 2026-08-30)

1. `libc/include/float.h` describes the target's IEEE-754 binary32 and
   binary64 formats and x87 80-bit `long double`, including normal and
   subnormal range constants, radix, precision, decimal conversion limits,
   rounding mode and evaluation method.
2. `math.h` and the libc math implementation add `fmod`, `remainder`,
   exponential and logarithmic families, `pow`, `hypot`, `cbrt`, trigonometric
   functions, NaN constructors, float variants, and classification of the
   native x87 `long double` representation. Range reduction combines explicit
   binary scaling with bounded polynomial or series evaluation; x87 remainder
   and trigonometric instructions are contained inside libc and do not change
   the kernel's no-floating-point rule.
3. `strtod` accepts C hexadecimal floating constants with a binary exponent,
   including exact binary range boundaries, and `strtof` reports binary32
   overflow and underflow. Decimal conversion, infinity, NaN and end-pointer
   behavior from M27 remain covered.
4. The public SSE2 interface adds masks, absolute value, square root, native
   minimum and maximum, clamp operations, and bulk multiply, scale and square
   root routines for float and double arrays. Vector loops retain scalar tails
   and make no alignment assumption.
5. `docs/design/floating.md` records the algorithms, exception and signed-zero
   behavior, native SSE minimum/maximum semantics, accuracy limits and the
   remaining work for inverse functions, a long-double function family,
   floating environment control, hexadecimal formatting and AVX state.

Tests: the expanded `float` case checks constants, extended math, special
values, signed zero, x87 classification, hexadecimal parsing and the new SIMD
operations; `mathvec` checks composed numerical identities across two hundred
binary exponents, exact parser boundaries and packed/array vector pipelines;
`fpu` continues to verify context switching, fork and signals. Clean target
builds, host framework checks, SSE2/AVX disassembly checks and the complete boot
suite pass (66 passed, 0 failed).

## M29. Complete scalar math families and floating environment (completed 2026-08-30)

1. The public scalar math interface gains inverse trigonometric functions
   (`asin`, `acos`, `atan`, `atan2`) and direct and inverse hyperbolic
   functions (`sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`), with float,
   double and long-double entry points and explicit domain, pole, infinity,
   NaN and signed-zero handling.
2. Every scalar family exposed by `math.h` now has an x87 binary80 variant.
   Long-double classification, rounding, decomposition, scaling, remainder,
   exponential, logarithmic, power, root and norm operations retain the
   64-bit binary80 significand instead of narrowing through double.
3. Trigonometric reduction uses a split pi/2 constant for small inputs and a
   16,512-bit fixed-point 2/pi table for Payne-Hanek reduction of large inputs.
   It covers every finite binary80 exponent, extracts the quotient quadrant as
   an integer, and evaluates binary80 sine/cosine kernels on [-pi/4, pi/4].
4. `fenv.h` and its libc implementation manage x87 and MXCSR together:
   exception clearing, testing, raising and flag images; all four rounding
   modes; environment get/set, save with masking and update; and the default masked,
   round-to-nearest-even environment.
5. The printf family gains `%a` and `%A`. Bit-based conversion emits exact
   normal and subnormal double values, preserves binary80 precision for `%La`,
   supports width, sign, padding, alternate form and precision, and follows
   the active floating rounding mode when precision removes significand bits.
6. `docs/design/floating.md` records the binary80 algorithms, full-range
   reduction, floating-environment state model and hexadecimal formatting.

Tests: `libmfull` exercises every new family and public long-double variant,
domain and pole behavior, binary80 precision, reference trigonometric values
at `1e300` and `2^16000`, x87/MXCSR rounding and exception semantics, and
`%a`/`%A`/`%La`. The existing `float`, `mathvec` and `fpu` cases remain green;
clean target builds, host framework checks, exported-symbol and instruction
checks, and the complete boot suite pass (67 passed, 0 failed).

## M30. RPN and algebraic scientific calculator (completed 2026-08-31)

1. `/bin/calc` is a native `libgui` application whose default mode is a
   32-level reverse Polish notation stack. Its display exposes T, Z, Y and X;
   `ENTER`, `SWAP`, `DROP`, `CE` and `AC` provide stack and entry control.
2. Arithmetic, remainder, power, reciprocal, constants and the scalar
   trigonometric, hyperbolic, logarithmic, exponential and root functions use
   the user-space floating runtime from M27-M29. Domain, range, zero-divisor,
   stack-depth and input-length failures are reported without discarding
   recoverable stack operands.
3. The input-mode selector changes the window to algebraic editing. A
   recursive-descent parser implements parentheses, function calls,
   comma-separated binary functions, constants, conventional precedence,
   signed operands and right-associative exponentiation. Changing modes carries
   X into the expression or an evaluated expression back into X.
4. The launcher lists Calculator, the canvas accepts direct keyboard input,
   and `docs/design/calculator.md` records the input models, operations,
   transitions and error behavior.

Tests: `tests/cases/calculator` runs `calc --self-test` and covers RPN entry,
stack manipulation and scientific operations together with algebraic
precedence, functions, constants and rejected syntax. `tests/cases/gui_calc`
starts the compositor, drives both input modes through actual keyboard and
combo-box events, checks the reported results, and closes the window. Host
framework checks pass and the complete boot suite passes (69 passed, 0 failed).

## M31. Audio playback stack (completed 2026-09-02)

1. A kernel PCM layer registers exclusive raw playback devices and defines a
   versioned capability, configuration, lifecycle and status ABI.  Exact-period
   blocking and nonblocking writes integrate with `poll`; the poll notification
   generation closes the readiness-scan-to-sleep race for all descriptor types.
2. The virtio-snd driver selects a 48 kHz stereo S16 output stream, implements
   control and TX virtqueues, manages two to eight reusable DMA periods, and
   reports playback position, queued frames, transfer errors and underruns.
3. `/bin/audiod` owns `/dev/pcm0`.  Its output-device clock drives a fixed-
   quantum mixer over any number of playback streams.  Per-client memfd pools,
   explicit buffer ownership, control events and per-stream gain preserve the
   useful server/data-plane separation of PipeWire without introducing a
   general graph or session manager.
4. `protocol/audio.xml` and generated libwire bindings define stream creation,
   configuration, queueing, state, drain and xrun messages.  `libaudio` wraps
   registry discovery, descriptor passing, mapping, buffer recycling and event
   dispatch; `playtone` provides a minimal client, while `synth` provides an
   interactive subtractive-synthesis client with an ADSR envelope, an
   envelope swept resonant low-pass filter and an oscilloscope.
5. The desktop-session supervisor starts and stops `audiod` when a PCM device
   is present. `make run` attaches virtio-snd and selects Core Audio by default
   on macOS, while other hosts retain the silent backend until configured.
   `docs/design/audio.md` records the boundaries and
   future extension points.

Tests: `audio_pcm` validates the raw device, configuration failures, exact
periods, readiness, drain and frame accounting.  `audio_server` plays two
concurrent shared-memory streams through `libaudio`, validates server shutdown,
and checks the emitted WAV payload for stereo 16-bit nonzero mixed samples.
The complete QEMU suite passes with 72 tests and no failures.

## M32. virtio-gpu display and virtio-input tablet (completed 2026-09-02)

1. A display layer in the kernel: `fb_screen` describes the active
   framebuffer (the Limine one until a GPU driver takes over), the console
   and `/dev/fb0` draw into it, and a GPU driver registers flush and mode
   set operations. `struct fb_info` gains `caps` and `size`; `FBIO_FLUSH`
   pushes a rectangle to the host and `FBIO_SET_MODE` changes the
   resolution and the pixel scale at run time for the display owner.
2. The virtio-gpu driver (`virtio-vga` in QEMU) owns one 16 MiB guest
   buffer for every mode, creates a 2D resource per mode, sets the
   scanout and transfers damaged rectangles. The console accumulates a
   dirty rectangle under `console_lock` that a kernel thread flushes
   every 20 ms; the panic path flushes by polling the used ring.
3. The virtio-input driver takes the tablet's absolute events, buttons
   and wheel and delivers them through the shared `/dev/mouse` ring
   (`drivers/mouse.c`, also used by the PS/2 driver) as events flagged
   `MOUSE_ABSOLUTE`. The QEMU window then needs no mouse grab.
4. The compositor positions the cursor from absolute events, applies the
   `display_mode` setting (`WxH@S` packed in one integer) by setting the
   mode, reallocating its back buffer and re-announcing the output to
   every client, and re-layouts layer surfaces and clamps windows. The
   desktop applies `display_mode` from `/etc/desktop.conf`; the settings
   application gets a Display page.
5. `tools/run.sh` attaches `-vga virtio` and a virtio tablet by default
   (`--vga std`, `--no-tablet` restore the old machine); the test runner
   takes `vga` and `tablet` case files. `docs/design/display.md`.

Tests: `gpu_mode` (virtio-vga: takeover at boot, flush, mode change with
the console following), `input_tablet` (absolute events through the ring
and the real device's probe), `gui_tablet` (the compositor's cursor
follows absolute events), and the GUI cases run on virtio-vga. The
complete QEMU suite passes with 76 tests and no failures.

## M33. Toolkit side HiDPI (completed 2026-09-03)

The output announces scale 2 when the mode has a pixel scale; libgui
renders at that scale (fonts, metrics, images) into buffers with
`set_buffer_scale(2)`; the compositor composes in device pixels, copying
scaled buffers 1:1 and doubling unscaled ones, and draws decorations and
the cursor at the scale. Text and icons become sharp on high density
displays. Tests: `gui_scale2` (a scaled client's buffer appears 1:1, an
unscaled client's buffer is doubled, decorations are drawn at scale) and
`gui_term_scale2` (the terminal, whose 18 MiB buffer pool needs the
raised shared memory limit, at scale 2). The complete QEMU suite passes
with 78 tests and no failures.

## M34. Audio follow-ups (completed 2026-09-03)

1. Capture through the virtio-snd RX queue: the driver (split into
   `virtio_snd.c` and `virtio_snd_stream.c`) runs a capture stream next to
   the playback stream on the same exclusive `/dev/pcm0`; the ABI (version 2,
   `AUDIO_CAP_CAPTURE`) adds the `AUDIO_*CAPTURE*` requests, reads of exactly
   one period and `POLLIN`.  Every capture period is submitted at prepare and
   resubmitted after it is read; a drop releases the stream and waits a
   bounded time for the device to hand the periods back, so host backends
   without a capture voice (`wav`, Core Audio) cannot stall the server.
2. `audiod` (split into `main.c`, `stream.c` and `control.c`) adds capture
   streams from two sources, the device input (through an input ring read on
   `POLLIN`) and the monitor of the mix, both clocked by the output; a master
   volume; and the `audio_control` interface: the stream list with volumes
   and states, master and per-stream volume requests and peak levels.
3. `libaudio` adds `audio_capture_*` (blocking reads with partial buffer
   consumption), `audio_mixer_*` (the mixer view) and
   `audio_connection_sync`.
4. The panel's audio applet (`user/panel/mixer.c`): a speaker button opens a
   popup with the master volume and one row per stream (name, state, volume
   bar, peak meter), set by clicking or dragging.
5. `/bin/player` plays PCM WAV files of any rate with a waveform view, play
   head, seeking, loop and volume; `.wav` files open with it from the file
   manager.  `/bin/sequencer` is a sixteen step, eight note polyphonic step
   sequencer on the synthesizer voice, which moved to
   `user/apps/synthvoice.h` and is shared with `/bin/synth`.
   `docs/design/audio.md` describes all of it.

Tests: `audio_capture` (raw capture and, through `audiod`, a tone played
back through the monitor source sample-exact, silent input periods, partial
reads and xruns for a late reader), `audio_mixer` (the mixer view from a
second connection with volume changes verified through the monitor
capture), `audio_player`, `audio_sequencer` and `gui_mixer` (the desktop
applications and the panel applet against the server); `audio_pcm` also
covers a capture drop under the `wav` backend.  The GUI test helpers moved
to `kernel/tests/gui_helpers.h` for `test_audio_gui.c`.
