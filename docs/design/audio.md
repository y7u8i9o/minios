# Audio stack

MiniOS provides one low-level PCM device with a playback and a capture
side, and a user-space audio server.  The internal format is fixed at
48 kHz, stereo, signed 16-bit little-endian samples.  This retains the
driver and real-time path small while establishing interfaces that can
later support more devices and formats.

## Architecture

The data path is:

```
applications -> libaudio -> shared memfd pools -> audiod mixer
             -> /dev/pcm0 -> virtio-snd TX virtqueue -> QEMU audio backend

QEMU audio backend -> virtio-snd RX virtqueue -> /dev/pcm0 (read side)
                   -> audiod input ring -> capture streams -> libaudio
                                            ^
                              the mix output (monitor source)
```

The design takes several useful ideas from PipeWire: the hardware endpoint is
owned by one server, clients exchange payloads through shared
memory, buffer ownership is explicit, protocol traffic remains off the sample
data path, the output device supplies the scheduling clock, and the mix
sent to the output is available again as a monitor source.  The current
server reduces a general processing graph to the topology MiniOS needs:
several playback streams feeding one mixer and one sink, and one input plus
the monitor feeding any number of capture streams.  It has no node/port
graph, session manager, dynamic format negotiation, sample-rate conversion,
effects, or network transport.

## Kernel PCM interface

`kernel/audio/pcm.c` registers flat devfs nodes through `struct pcm_device`.
The first virtio-snd device appears as `/dev/pcm0`; opening it is exclusive
so that mixing and policy remain in `audiod`.  One open descriptor carries
both directions: writes feed the playback stream, reads take from the
capture stream.

The public ABI in `sys/audio.h` (`minios/abi.h`, version 2) supports
capability discovery, parameter selection, prepare, start, drop, drain and
status requests.  The playback requests are `AUDIO_SET_PARAMS`,
`AUDIO_PREPARE`, `AUDIO_START`, `AUDIO_DROP`, `AUDIO_DRAIN` and
`AUDIO_GET_STATUS`; a device that reports `AUDIO_CAP_CAPTURE` accepts the
same sequence for its capture stream through `AUDIO_SET_CAPTURE_PARAMS`,
`AUDIO_CAPTURE_PREPARE`, `AUDIO_CAPTURE_START`, `AUDIO_CAPTURE_DROP` and
`AUDIO_GET_CAPTURE_STATUS`.  The two streams are configured and run
independently.  The state sequence of each is:

```
CLOSED -> OPEN -> PREPARED -> RUNNING -> OPEN -> CLOSED
```

An error moves the stream to `ERROR`; drop or close releases it.  Writes and
reads are exactly one configured period.  Blocking calls wait for a free
(playback) or full (capture) period, while nonblocking ones return
`EAGAIN`.  `poll` reports `POLLOUT` for a free playback period and `POLLIN`
for a captured one; virtqueue completions notify the common poll wait
queue.  In the capture status, `played_frames` counts captured frames,
`queued_frames` the captured frames not yet read, and `xruns` the periods
the device could not fill because every period was still waiting for the
reader.

The driver accepts 120 to 2048 frames per period and two to eight
periods.  `virtio_snd.c` probes the device, runs control queue 0 and the
ioctl interface; `virtio_snd_stream.c` runs the period rings on TX queue 2
and RX queue 3.  The driver selects the first output and the first input
stream advertising stereo S16 at 48 kHz and issues `SET_PARAMS`, `PREPARE`,
`START`, `STOP` and `RELEASE` per stream.  A playback period is submitted by
`write` and returns free with its used entry; every capture period is
submitted at prepare, returns full in submission order, and `read` copies
the oldest one out and submits it again.  Submitted chains remain
device-owned until their used entries arrive; therefore a playback drop
lets the small hardware queue finish before `STOP`, while a capture drop
sends `STOP` and `RELEASE`, which hands the pending periods back, and waits
a bounded time for them.  A period the device retains beyond that remains
allocated and is ignored through its generation when it finally returns.
The device writes the capture status after the data it delivered, so a
short transfer (a flush at release) leaves the status structure untouched;
only a full period carries one.

QEMU's `wav` and Core Audio backends have no capture voice: the device
accepts the capture stream but never fills a period.  The read side then
simply never becomes readable, and the drop path still returns promptly.

## `audiod`

`/bin/audiod` opens `/dev/pcm0` nonblocking, configures a 480-frame quantum
(10 ms) with a four period device ring in both directions, primes the
playback ring with silence, and publishes the `audio_manager` global on the
`audio` Unix-domain socket.  `startgui` starts it when `/dev/pcm0` exists
and stops it with the rest of the desktop session.  The server is split
into the mixer and the device loop (`main.c`), the stream protocol
(`stream.c`) and the control interface (`control.c`).

Each stream receives a three-buffer memfd pool.  A playback buffer
alternates between client ownership and the server's queued state:

```
configured/ready -> client fills -> queue_buffer -> mixer consumes
                 <- buffer_ready <-
```

A capture buffer runs the other way: the server owns every buffer from
configuration on, fills one per period and sends `captured`; the client
returns it with `queue_buffer` once read.

The device ring is the output latency (40 ms): `POLLOUT` on `/dev/pcm0`
means that one period has been played, and the mixer answers with exactly one
mixed period, consuming at most one buffer from every active playback
stream.  Samples are multiplied by a per-stream Q16.16 gain, summed in
signed 32-bit accumulators, multiplied by the master gain, clamped to 16-bit
output and submitted to PCM.  The same period is then delivered to every
active capture stream: monitor streams receive the mix just produced,
input streams the oldest period of the input ring, which `POLLIN` on the
device fills with captured periods (up to four; the ring drops its oldest
when the mixer falls behind), or silence when the ring is empty.  Capture
streams are therefore clocked by the output like everything else, and a
host backend without a capture voice yields silence rather than a stall.
When the whole playback ring has drained (the server was late by four
periods), the device has been playing silence; the server refills the ring
with silence and mixes only the last period, so clients are never asked for
more periods than time has passed and a stall does not turn into a burst of
xruns.

A stream that has not exchanged a buffer since activation is pre-rolling
and misses periods silently.  Afterwards every period at which a playback
stream has no queued buffer, or a capture stream has no buffer to fill, is
an xrun: the server outputs silence for it (or drops the period) and sends
an `xrun` event with the running count.  Drain stops a playback stream
after its final queued buffer and reports `drained`; other streams continue
running.

Malformed configuration or buffer-ownership requests are protocol errors and
disconnect the offending client.  This confines stream state and shared maps
to the client that created them.

### The control interface

`audio_manager.get_control` creates an `audio_control` object, the mixer
view of the server: it announces the master volume and every configured
stream (a server-wide id, the name, direction, Q16.16 volume and running
state), reports later changes with the same `stream` event, removals with
`stream_removed`, and, while any control object exists, the peak sample of
every running stream every five periods (`level`).  Peaks are measured on
the stream's own samples before its gain, so a meter shows what the client
produces.  `set_stream_volume` and `set_master_volume` change gains from
outside the owning client; a client's own `set_volume` shows up in every
view as well.

## Client interface

`libaudio` hides registry discovery, descriptor passing, mapping and buffer
recycling.  Its public interface is `audio/audio.h`.  The normal playback
sequence is:

1. `audio_connect()` and `audio_playback_create()`.
2. `audio_playback_start()`.
3. Repeated `audio_playback_write()` calls with interleaved stereo frames.
4. `audio_playback_drain()` and `audio_disconnect()`.

Capture mirrors it: `audio_capture_create()` with a source
(`AUDIO_SOURCE_INPUT` or `AUDIO_SOURCE_MONITOR`), `audio_capture_start()`,
then `audio_capture_read()`, which blocks for captured frames, consumes a
buffer partially when asked for fewer frames than it contains and returns
each buffer to the server once it is drained.  `audio_capture_available()`
reports the frames an event driven client may read without blocking.
`audio_mixer_create()` returns the mixer view: a list of
`struct audio_mixer_stream` with names, volumes in percent, states and
peaks, the master volume, a generation counter that changes with every
update, and setters for the master and per-stream volumes.
`audio_connection_sync()` is a round trip; a client that inspects the view
through a second connection syncs the first one before the view, since
the server handles the two sockets independently.

The connection descriptor and dispatch function are also public so a GUI or
event-driven application can include audio events in its own poll loop.  Such
a client must refill every buffer reported by `audio_playback_ready()` each
time the connection becomes readable, not one buffer per wakeup: the server
retains a full three-buffer pool for it and the client survives the device
returning several periods at once.  Writing one buffer per wakeup leaves a
single period of headroom and produces a 10 ms gap whenever two periods
complete together.  `audio_connection_dispatch()` with a timeout of 0 takes
what has already arrived on the socket without waiting.

## Applications

`/usr/bin/playtone` is a minimal client that emits a one-second triangle wave.

`/usr/bin/synth` is a small monophonic subtractive synthesizer: a saw, square or
triangle oscillator with a five octave range feeds an ADSR amplitude
envelope and a resonant state-variable low-pass filter whose cutoff is swept
by the envelope; a cubic soft clipper follows.  The filter runs twice per
sample so that it remains stable up to the highest cutoff the sweep reaches.
The voice is defined in `user/apps/synthvoice.h` (`struct synth_voice`,
`synth_voice_on`, `synth_voice_off`, `synth_voice_sample` with coefficients
derived from `struct synth_params`) so that other programs can play it.
The window exposes oscillator, octave, cutoff, resonance, envelope amount,
volume and ADSR controls, an oscilloscope of the most recent period and
clickable chromatic note pads (keys A to K play, Z and X shift the octave).
Audio runs from the application event loop through `fill_ready_buffers()`,
which refills every returned buffer, and the phase continues across
retriggers so note changes do not click.

`/usr/bin/sequencer` is a step sequencer on the same voice: sixteen steps by
eight notes of a major scale, one voice per row so that chords play
polyphonically.  The transport runs on the audio clock (the render loop
counts frames per step), with tempo, octave, waveform, cutoff, resonance,
decay and volume controls; the gate of a step closes halfway through it so
that repeated notes are heard separately.  Space plays and stops, D loads
the demo pattern, C clears the grid, and a click toggles a cell.

`/usr/bin/player` plays every audio format that a codec module
decodes (`codecs.md`), for example PCM WAV files with 8, 16, 24 or 32-bit
samples at any sample rate.  When a file is opened, a loader thread
decodes it through libcodec, retains the upper 16 bits of each sample, and
converts the result to 48 kHz stereo.  The player outputs a mono file on
both channels and outputs only the first two channels of a file with
more channels.  The player draws the whole file
as a waveform of the minimum and the maximum in each pixel column, with a
vertical line at the play position.  A click on the waveform sets the play
position, space plays and pauses, and the Loop check box repeats the file.
`/etc/mime.types` maps `.wav` to `audio/x-wav`, and `/etc/mime.apps` opens
`audio/*` with the player.  `/usr/share/sounds/chime.wav` is a sample file
at 16 kHz.

The player converts other sample rates by linear interpolation between the
two nearest source frames.  The option `-s` and the Resampling menu select an
experimental sinc resampler.  A selection in the menu loads the current file
again and continues playback at the same position.  The sinc resampler
weights the source frames within 16 zero crossings on either side of the
output position with a sinc kernel and a Blackman window.  The cutoff is 0.95
of the lower of the source and the output Nyquist frequencies.  The kernel
therefore covers more source frames when the source rate is above 48 kHz.
The coefficients are computed in double precision for 4097 fractional
positions when a file is loaded, scaled to a sum of 1 and stored as 32-bit
integers with 15 fraction bits.  Each output frame uses the coefficients of
the nearest fractional position and a 64-bit accumulator.  The information
line and the message `player: loaded` on standard output name the method,
`linear` or `sinc`, when the file rate differs from 48 kHz.

The following table lists the signal-to-noise ratio of a 16000-amplitude
sine tone after conversion to 48 kHz, measured on the host with the two
functions of `user/apps/player.c`.

| Source rate | Tone | Linear | Sinc |
|---|---|---|---|
| 16000 Hz | 1000 Hz | 37.1 dB | 86.6 dB |
| 16000 Hz | 5000 Hz | 9.9 dB | 75.9 dB |
| 16000 Hz | 7000 Hz | 4.7 dB | 21.6 dB |
| 44100 Hz | 1000 Hz | 54.6 dB | 84.4 dB |
| 44100 Hz | 7000 Hz | 21.0 dB | 80.7 dB |
| 96000 Hz | 5000 Hz | 93.7 dB | 85.6 dB |

A table of 257 positions limited the sinc resampler to between 53 and 76 dB
in the same measurement.  The table of 4097 positions requires 557 KB for a
44.1 kHz file and 1.1 MB for a 96 kHz file.  For a 16 kHz file the
transition band of the kernel is centred on the 7600 Hz cutoff.  The kernel
attenuates the 7000 Hz tone because the tone is inside this band.  The rate
96 kHz is twice the output rate.  Linear interpolation then copies every
second source frame and adds no error for tones below 24 kHz.

The panel's audio applet (`user/panel/mixer.c`) is a speaker button left of
the clock.  It opens a popup that connects to the server, shows the master
volume and one row per stream with its name, state, volume bar and peak
meter, and sets volumes by clicking or dragging a bar; the popup retains the
size it opened with and counts rows beyond it.  The connection is closed
with the popup.

## Running and tests

`make run` attaches virtio-snd and selects QEMU's Core Audio backend on macOS,
so desktop clients are audible on the host. Other hosts retain the silent
`none` default until a backend is selected explicitly. The backend, extra
`-audiodev` properties and the device itself are configured through
`tools/run.sh`: `make QEMU_AUDIO=none run` disables host playback,
`make RUNFLAGS="--audio wav" run` records the mix to `build/audio.wav`,
`--no-sound` boots without the device, and a `qemu.conf` in the repository
root makes a choice permanent (see `docs/design/build.md`). QEMU's Core Audio
backend has no capture side, so it prints a warning about `virtio-sound.in`
at startup; the input source then yields silence and the monitor source
still works. The headless test runner accepts an `audio` case setting of
`none` or `wav`; `none` is the backend with a (silent) capture voice.

`audio_pcm` tests the raw device ABI, state changes, exclusive access,
nonblocking readiness, frame accounting and drain, and that a capture stream
under the `wav` backend, which never fills a period, still drops promptly.
`audio_server` starts `audiod`, creates two simultaneous `libaudio` streams
at different gains, plays and drains them, stops the server, and verifies
that QEMU produced a stereo 16-bit WAV file containing nonzero mixed samples.
`audio_capture` (backend `none`) reads periods from the raw capture stream
with poll and frame accounting, then plays a tone through `audiod` while a
monitor capture stream returns it sample-exact, an input capture stream
delivers silent periods at the same rate, partial reads consume buffers
piecewise, and a reader that stops returning buffers sees xruns.
`audio_mixer` checks the mixer view from a second connection: the stream
list with names, directions and states, per-stream and master volume
changes verified through the monitor capture of the mix, level events and
removal.  `audio_player`, `audio_sequencer` and `gui_mixer` run the desktop
applications against the server: the player draws the chime's waveform,
reports the end of the file and converts the chime a second time with
`-s`, the sequencer loads and plays the demo
pattern from the keyboard, and the panel applet lists the synthesizer's
stream and sets the master volume by a click on its bar.


## Lua binding

Lua programs load `require "audio"` to access libaudio connections,
playback, input and monitor capture, and mixer controls. The binding uses
binary stereo S16_LE PCM strings and integrates with GUI descriptor
watches. Objects support explicit close, garbage collection and Lua's
`<close>` scopes; a closed connection invalidates its child handles.
`lua.md` documents every method, error result and lifetime rule. The
example `/usr/share/lua/examples/tone.lua` generates and drains a tone.

The `lua_audio` boot test verifies the generated PCM through the real
server's monitor, partial input reads, mixer updates and stream cleanup.
`make check-lua` also runs deterministic binding tests with a host-only
backend, including errors and partial I/O that are difficult to provoke
reliably through a live device.

The independent `luasynth` package implements an eight-voice instrument
entirely in Lua, with two oscillators per voice, modulation, stereo delay
and saved patches (`luasynth.md`). It coexists with the C `synth` package.
