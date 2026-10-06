# The Intel HD Audio driver

Milestone S2 of `docs/plan/release-0.7.0.md` (D7 of `docs/plan/drivers.md`)
adds a driver for Intel High Definition Audio controllers in
`kernel/drivers/hda.c`. QEMU offers HD Audio as `intel-hda` (ICH6) and
`ich9-intel-hda`, combined with a codec such as `hda-duplex` or
`hda-output`, and UTM uses it as its default sound device on several machine
types. The driver follows the *High Definition Audio Specification*,
revision 1.0a. It registers a playback-only PCM device with the same
interface as virtio-snd (`audio.md`), so `audiod` and everything above it
work unchanged.

## Probe

`hda_init` runs in the start-up thread after `virtio_snd_init` and binds the
first PCI function of class 04, subclass 03. It maps BAR 0, enables bus
mastering and resets the controller through `GCTL.CRST`. After the reset the
codecs announce themselves in `STATESTS`.

The driver then sets up the two command rings in one DMA page each, using
the largest size the controller supports (256 entries on QEMU):

- **CORB** (command outbound ring buffer). The driver writes a 32-bit verb
  and advances `CORBWP`. The reset handshake of `CORBRP` is bounded but not
  required, because some controllers do not report the reset bit back.
- **RIRB** (response inbound ring buffer). The controller writes 64-bit
  responses and advances `RIRBWP`. The driver polls the write pointer
  rather than taking an interrupt, because codec commands are only sent
  during probe. Unsolicited responses are skipped. `RIRBCTL.RINTCTL` is
  set, so the controller flags every response in `RIRBSTS`, and the driver
  acknowledges the flag after each command: QEMU stops reading the CORB
  while the response count is reached and not acknowledged. No interrupt
  results, because `INTCTL.CIE` remains clear.

`command` serializes verbs with a mutex and waits up to 50 ms for each
response.

## Codec and output path

For each codec present, the driver looks for the first audio function group
under the root node, powers it up and reads its widgets: the capabilities,
the amplifier capabilities (from the widget or, without the override bit,
from the function group), the pin capabilities and default configuration of
pins, and the connection list, in short or long form with ranges expanded.

It then chooses an output pin. Pins whose default configuration says that
nothing is connected are skipped. Line out is preferred, then speakers,
then headphones, then any other output-capable pin. From the chosen pin a
depth-first search through mixers and selectors finds a DAC. Along the
path the driver:

- powers up every widget (D0);
- selects the next node of the path on selectors and multi-input pins;
- sets output amplifiers, and the input amplifier of the used mixer input,
  to 0 dB unmuted;
- enables the pin output, with the headphone amplifier if the pin has one,
  and enables EAPD where the pin supports it;
- sets the DAC to 48 kHz, 16-bit stereo and connects it to stream tag 1.

On QEMU's `hda-duplex` the path is pin 3 (line out) to DAC 2.

## Playback stream

Playback uses the first output stream descriptor, which follows the input
descriptors (`GCAP.ISS`). The audio buffer is a physically contiguous 64 KiB
block. At prepare it is divided into as many periods of the configured size
as fit, at most 256, and each period becomes one entry of the buffer
descriptor list with interrupt-on-completion set. This *hardware ring* is
usually much longer than the PCM ring that the caller configured:

- The PCM parameters (two to eight periods of 120 to 2048 frames) only limit
  how many periods may be queued at once.
- QEMU's codec fetches up to 8 KiB ahead of its audio backend in a single
  timer tick. With a hardware ring of only four 1920-byte periods, the
  controller could wrap around completely between two position checks, the
  driver would miss a cycle, and stale periods were played twice. The long
  hardware ring makes this impossible.

The driver tracks the period the controller is reading from `SDnLPIB`. When
the position moves past a period, that period is counted as played if it
held data, and it is zeroed. Zeroing is safe because the controller is
already reading a later period, and it means an underrun plays silence
instead of old samples. Writes fill periods strictly in ring order. When
nothing is queued on a running stream, the next write goes to the period
after the one being played.

Position updates come from the stream's interrupt through MSI. Without MSI,
or with the kernel option `hda=poll`, a kernel thread checks the position
every 2 ms, which is below the shortest period of 2.5 ms.

## PCM semantics

The device behaves as virtio-snd does for playback: writes are exactly one
period, `POLLOUT` reports room for a period, `AUDIO_START` requires two
queued periods, and an empty ring on a running stream counts as an xrun.
`AUDIO_GET_INFO` reports playback only, and the capture requests return
`ENODEV`.

`AUDIO_DRAIN` and `AUDIO_DROP` wait until every queued period has been
fetched. Stopping the stream then would discard what the codec has
buffered, which on QEMU is up to 8 KiB, or about 40 ms. The driver therefore
lets the stream run on through silent periods until at least that much more
has been fetched, and then stops the DMA engine. Both waits are bounded, so
a controller that stopped reporting positions cannot hang the caller.

## Tests

The `audio` file of a case takes an optional second word: `hda` attaches
`intel-hda` with `hda-duplex` instead of virtio-snd.

- `hda_pcm` runs `audiotest` against the HD Audio device. The post script
  checks that the recorded 440 Hz square wave lasts 0.24 s, the length that
  `audiotest` writes, and that no half-period of the wave is shortened or
  lengthened, which would indicate a repeated or skipped period.
- `hda_pcm_poll` runs the same test with `hda=poll`.
- `hda_audiod` runs the `audiod` integration test on HD Audio.

## Limits

- Playback only; the input converters of the codec are not used.
- 48 kHz, 16-bit stereo only, which is the format of `audiod`.
- One controller and one codec path. Jack detection, volume control
  through the codec amplifiers and HDMI audio are not implemented.
