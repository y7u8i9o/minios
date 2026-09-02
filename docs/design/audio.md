# Audio stack

MiniOS provides one low-level PCM playback device and a user-space audio
server.  The first implementation deliberately fixes the internal mix format
at 48 kHz, stereo, signed 16-bit little-endian samples.  This keeps the driver
and real-time path small while establishing interfaces that can later support
more devices and formats.

## Architecture

The data path is:

```
applications -> libaudio -> shared memfd pools -> audiod mixer
             -> /dev/pcm0 -> virtio-snd TX virtqueue -> QEMU audio backend
```

The design takes several useful ideas from PipeWire: the hardware endpoint is
owned by one server, clients exchange payloads through shared
memory, buffer ownership is explicit, protocol traffic stays off the sample
data path, and the output device supplies the scheduling clock.  The current
server reduces a general processing graph to the one topology MiniOS needs:
several playback streams feeding one mixer and one sink.  It has no node/port
graph, session manager, dynamic format negotiation, sample-rate conversion,
capture path, effects, or network transport.

## Kernel PCM interface

`kernel/audio/pcm.c` registers flat devfs nodes through `struct pcm_device`.
The first virtio-snd device appears as `/dev/pcm0`; opening it for playback is
exclusive so that mixing and policy remain in `audiod`.

The public ABI in `sys/audio.h` supports capability discovery, parameter
selection, prepare, start, drop, drain and status requests.  The state sequence
is:

```
CLOSED -> OPEN -> PREPARED -> RUNNING -> OPEN -> CLOSED
```

An error moves the stream to `ERROR`; drop or close releases it.  Writes are
exactly one configured period.  Blocking writes wait for a free period, while
nonblocking writes return `EAGAIN`.  `poll(POLLOUT)` reports period
availability, and virtqueue completion notifies the common poll wait queue.

The first driver accepts 120 to 2048 frames per period and two to eight
periods.  `virtio_snd.c` uses virtio-snd control queue 0 and playback TX queue
2.  It selects the first output stream advertising stereo S16 at 48 kHz,
issues `SET_PARAMS`, `PREPARE`, `START`, `STOP` and `RELEASE`, and accounts
played frames, queued frames, transfer errors and underruns.  Submitted TX
chains remain device-owned until their used entries arrive; therefore drop
allows the small hardware queue to finish before sending `STOP`, while data
that has not reached the driver is discarded by the caller.

## `audiod`

`/bin/audiod` opens `/dev/pcm0` nonblocking, configures a 480-frame quantum
(10 ms) with a four period device ring, primes the ring with silence, and
publishes the `audio_manager` global on the `audio` Unix-domain socket.  `startgui` starts it
when `/dev/pcm0` exists and stops it with the rest of the desktop session.
Keeping it in the session supervisor prevents service output from racing with
the interactive shell prompt.

Each playback stream receives a three-buffer memfd pool.  A buffer alternates
between client ownership and the server's queued state:

```
configured/ready -> client fills -> queue_buffer -> mixer consumes
                 <- buffer_ready <-
```

The device ring is the output latency (40 ms): `POLLOUT` on `/dev/pcm0`
means that one period has been played, and the mixer answers with exactly one
mixed period, consuming at most one buffer from every active stream.  Samples
are multiplied by a per-stream Q16.16 gain, summed in signed 32-bit
accumulators, clamped to 16-bit output, and submitted to PCM.  When the whole
ring has drained (the server was late by four periods), the device has been
playing silence; the server refills the ring with silence and mixes only the
last period, so clients are never asked for more periods than time has passed
and a stall does not turn into a burst of xruns.

A stream that has not delivered a buffer since activation is pre-rolling and
misses periods silently.  Afterwards every period at which the stream has no
queued buffer is an xrun: the server outputs silence for it and sends an
`xrun` event with the running count.  Drain stops that stream after its final
queued buffer and reports `drained`; other streams continue running.

Malformed configuration or buffer-ownership requests are protocol errors and
disconnect the offending client.  This confines stream state and shared maps
to the client that created them.

## Client interface

`libaudio` hides registry discovery, descriptor passing, mapping and buffer
recycling.  Its public interface is `audio/audio.h`.  The normal sequence is:

1. `audio_connect()` and `audio_playback_create()`.
2. `audio_playback_start()`.
3. Repeated `audio_playback_write()` calls with interleaved stereo frames.
4. `audio_playback_drain()` and `audio_disconnect()`.

The connection descriptor and dispatch function are also public so a GUI or
event-driven application can include audio events in its own poll loop.  Such
a client must refill every buffer reported by `audio_playback_ready()` each
time the connection becomes readable, not one buffer per wakeup: the server
keeps a full three-buffer pool for it and the client survives the device
returning several periods at once.  Writing one buffer per wakeup leaves a
single period of headroom and produces a 10 ms gap whenever two periods
complete together.
`/bin/playtone` is a minimal client that emits a one-second triangle wave.
`/bin/synth` is a small monophonic subtractive synthesizer: a saw, square or
triangle oscillator with a five octave range feeds an ADSR amplitude
envelope and a resonant state-variable low-pass filter whose cutoff is swept
by the envelope; a cubic soft clipper follows.  The filter runs twice per
sample so that it stays stable up to the highest cutoff the sweep reaches.
The window exposes oscillator, octave, cutoff, resonance, envelope amount,
volume and ADSR controls, an oscilloscope of the most recent period and
clickable chromatic note pads (keys A to K play, Z and X shift the octave).
Audio runs from the application event loop through `fill_ready_buffers()`,
which refills every returned buffer, and the phase continues across
retriggers so note changes do not click.

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
at startup; the guest only uses playback. The headless test runner accepts an
`audio` case setting of `none` or `wav`.

`audio_pcm` tests the raw device ABI, state changes, exclusive access,
nonblocking readiness, frame accounting and drain.  `audio_server` starts
`audiod`, creates two simultaneous `libaudio` streams at different gains,
plays and drains them, stops the server, and verifies that QEMU produced a
stereo 16-bit WAV file containing nonzero mixed samples.
