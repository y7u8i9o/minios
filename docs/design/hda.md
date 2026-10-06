# The Intel HD Audio driver

Milestone S2 of `docs/plan/release-0.7.0.md` (D7 in `docs/plan/drivers.md`)
adds a driver for Intel High Definition Audio controllers in
`kernel/drivers/hda.c`. QEMU emulates these controllers as `intel-hda` (ICH6)
and `ich9-intel-hda`, paired with a codec such as `hda-duplex` or
`hda-output`. The driver is based on the *High Definition Audio
Specification*, revision 1.0a. It registers a playback-only PCM device that
behaves like the virtio-snd device described in `audio.md`, so `audiod` and
the applications above it need no changes.

## Probe

`hda_init` runs in the start-up thread after `virtio_snd_init` and takes the
first PCI function with class 04h, subclass 03h. It maps BAR 0, enables bus
mastering and resets the controller by toggling `GCTL.CRST`. When the reset
completes, each attached codec sets its bit in `STATESTS`.

The driver then sets up the two command rings, each in its own DMA page and
at the largest size the controller supports (256 entries on QEMU):

- The **CORB** (command outbound ring buffer) carries verbs to the codecs.
  To send a verb, the driver writes it to the next entry and advances
  `CORBWP`. The specification describes a handshake for resetting `CORBRP`;
  the driver waits for it only briefly, because some controllers never
  report the reset bit back.
- The **RIRB** (response inbound ring buffer) carries the codec responses
  back. Codec commands are sent only during probe, so the driver polls
  `RIRBWP` instead of using an interrupt, and skips unsolicited responses.
  It does set `RIRBCTL.RINTCTL` and acknowledges the resulting status flag
  in `RIRBSTS` after every response, because QEMU stops processing the CORB
  once the response count is reached and the flag is still set. Since
  `INTCTL.CIE` is never enabled, this does not generate interrupts.

`command` serializes verbs with a mutex and allows 50 ms for each response.

## Codec and output path

For each codec that is present, the driver finds the first audio function
group below the root node, powers it up and reads its widgets. For every
widget it records the capabilities, the amplifier capabilities (taken from
the function group unless the widget overrides them), the pin capabilities
and default configuration of pins, and the connection list. Long-form
connection lists and ranges are supported.

Next it chooses an output pin, ignoring pins whose default configuration
marks them as unconnected. Line out is preferred, followed by speakers,
headphones and any other output-capable pin. A depth-first search from the
chosen pin through mixers and selectors then finds a DAC. For every widget
on that path, the driver:

- switches it to power state D0;
- selects the next widget of the path, if the widget is a selector or a pin
  with several inputs;
- sets its output amplifier, and the input amplifier of the mixer input in
  use, to 0 dB and unmutes it;
- enables the output of the pin, including the headphone amplifier where the
  pin has one, and turns on EAPD where the pin supports it;
- configures the DAC for 48 kHz 16-bit stereo and assigns it stream tag 1.

With QEMU's `hda-duplex` codec, the path runs from pin 3 (line out) to
DAC 2.

## Playback stream

Playback uses the first output stream descriptor, which follows the input
descriptors (their number is in `GCAP.ISS`). The audio buffer is a single
physically contiguous 64 KiB block. When the stream is prepared, the driver
divides the buffer into as many periods of the configured size as will fit,
up to 256, and creates one buffer descriptor list entry per period with
interrupt-on-completion enabled. The controller plays this hardware ring in
a loop.

The hardware ring is normally much longer than the ring that the PCM
parameters describe. Those parameters (two to eight periods of 120 to 2048
frames) only limit how many periods a client may have queued at once. The
reason for the longer ring is QEMU's codec, which can fetch up to 8 KiB
ahead of its audio backend in a single timer tick. With a ring of four
1920-byte periods, the controller sometimes wrapped around completely
between two position checks. The driver then missed a whole cycle and the
old periods were played a second time. A 64 KiB ring cannot wrap within one
such step.

The driver follows the controller's progress through `SDnLPIB`. Whenever
the position moves past a period, that period is counted as played if it
contained data, and its samples are cleared to zero. Clearing is safe at
that point because the controller has already moved on to a later period,
and it means that an underrun produces silence rather than stale audio.
Periods are filled strictly in ring order. If nothing is queued while the
stream is running, the next write goes to the period after the one the
controller is currently playing.

Position updates normally arrive through the stream interrupt, delivered
by MSI. If MSI is unavailable, or the kernel option `hda=poll` is given, a
kernel thread checks the position every 2 ms instead. That interval is
shorter than the smallest period, which lasts 2.5 ms.

## PCM semantics

For playback the device follows the same rules as virtio-snd: each write
must contain exactly one period, `POLLOUT` means a period can be written,
`AUDIO_START` requires two queued periods, and running out of queued
periods while the stream runs counts as an xrun. `AUDIO_GET_INFO` reports
playback only, and the capture requests fail with `ENODEV`.

`AUDIO_DRAIN` and `AUDIO_DROP` first wait until the controller has fetched
every queued period. Stopping the stream at that moment would throw away
whatever the codec still has buffered, which on QEMU can be up to 8 KiB, or
roughly 40 ms of audio. The driver therefore keeps the stream running over
silent periods until at least that much additional data has been fetched,
and only then stops the DMA engine. Both waits have time limits, so a
controller that stops reporting its position cannot block the caller
indefinitely.

## Tests

The `audio` file of a test case accepts an optional second word. With `hda`,
the harness attaches `intel-hda` and `hda-duplex` instead of virtio-snd.

- `hda_pcm` runs `audiotest` on the HD Audio device. Its post script checks
  that the recorded 440 Hz square wave lasts 0.24 s, which is exactly what
  `audiotest` writes, and that every half-period of the wave has the
  expected length. A repeated or skipped period would show up as a
  half-period that is too short or too long.
- `hda_pcm_poll` runs the same test with `hda=poll`.
- `hda_audiod` runs the `audiod` integration test on HD Audio.

## Limits

- Only playback is supported; the codec's input converters are unused.
- The only format is 48 kHz 16-bit stereo, which is the format `audiod`
  uses.
- The driver uses one controller and one output path. Jack detection,
  volume control through the codec amplifiers and HDMI audio are not
  implemented.
