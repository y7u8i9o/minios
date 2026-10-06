# Release 0.7.0: devices and desktop services

This plan covers release 0.7.0 of `docs/plan/roadmap.md`. Milestone
identifiers use the prefix `S`. Each milestone ends with boot tests, adds or
extends the design documents it lists, and is marked completed here once its
boot tests pass. The owner decided the open questions of scope on
2026-10-06; they are recorded in section 2.

## 1. Motivation and scope

Releases 0.5 and 0.6 made minios boot on more machine configurations and
cooperate with its host. Two device classes that QEMU and UTM commonly offer
are still unsupported: the Intel e1000e network card and Intel HD Audio.
On the desktop side, minios lacks several services that users expect from
any desktop environment: there is no way for programs to post
notifications, the session cannot be locked, all windows share a single
desktop, and windows can only be arranged by hand. A few everyday
applications are also missing: there is no archive manager or PDF viewer,
deleted files are gone immediately, and the file search in Files matches
names only.

After this plan:

- minios drives the e1000e network card and Intel HD Audio controllers.
- libcodec decodes Opus in Ogg and AAC-LC in MP4 and ADTS files.
- Programs post notifications, which appear as pop-ups and remain in a
  history in the panel.
- The session locks on request and after a period of inactivity, and only
  the user's password unlocks it.
- The desktop has several workspaces, and windows snap to screen edges
  and halves.
- An archive manager opens, extracts and creates tar and gzip archives.
- A PDF viewer displays PDF documents with their embedded fonts and images.
- Files moves deleted items to a trash and searches file contents.

The release is large, so the owner asked for it to be implemented in two
halves. The first half is S1 to S6; the second half, S7 to S12, follows in
a later session.

## 2. Fixed decisions

- The AAC-LC decoder is written for minios rather than ported. FAAD2 is
  licensed under the GPL and fdk-aac under a licence that is incompatible
  with the GPL, and either would constrain the licence of libcodec. The
  patent status is checked before the module enters a release, as section 6
  of the roadmap requires.
- The Opus decoder is a port of the reference implementation, libopus
  (BSD licence), restricted to decoding.
- The PDF renderer is written for minios. MuPDF is licensed under the AGPL
  and PDFium needs a C++ runtime, which minios does not have. The renderer
  covers what ordinary documents use (section 3, S9); encryption, forms,
  annotations other than links, JavaScript, transparency groups and
  shading types 4 to 7 are out of scope.
- Notifications are served by a session daemon over libwire, in the
  manner of the freedesktop notification specification: a summary, a
  body, an application name, an icon, an urgency, a timeout and up to three
  actions.
- The screen locker follows the model of the Wayland `ext-session-lock`
  protocol: X12 itself guarantees that the lock surface covers every output
  and receives all input, so a crash of the locker leaves the session
  locked rather than exposed.
- The trash follows the freedesktop Trash specification
  (`~/.local/share/Trash` with `files/` and `info/`).

## 3. Milestones

### S1. The e1000e network card (D6) (completed 2026-10-06)

A driver for the Intel 82574L, which QEMU emulates as `e1000e`. It uses
one receive and one transmit ring with legacy descriptors, reads the MAC
address from the EEPROM, tracks the link state, and registers the next free
`ethN` interface with the network core. The interrupt arrives through MSI;
without MSI, or with the kernel option `e1000e=poll`, a timer of the
network worker polls the rings. Network drivers now register a service
function with the worker instead of being called from it by name.

Boot tests: `net_e1000e` exchanges 640 raw frames with the echo peer,
checks the MAC address and the link state, and resets the controller.
`net_e1000e_poll` runs the same test without an interrupt.
`net_e1000e_dhcp` configures the interface through DHCP with QEMU's user
mode network.

Document: `docs/design/e1000e.md`, `docs/design/network.md`.

### S2. Intel HD Audio (D7) (completed 2026-10-06)

A driver for Intel HD Audio controllers: the CORB and RIRB command rings,
codec enumeration through the widget graph, an output path from a DAC to
an output pin, and an output stream descriptor with a buffer descriptor
list. The driver registers a playback PCM device with the kernel audio
layer, so `audiod` uses it in the same way as virtio-snd. Position updates
arrive through MSI, or from a polling thread with `hda=poll`.

Boot tests: `hda_pcm` plays a test tone through `intel-hda` with
`hda-duplex` and the wav audio backend, and the post script checks the
frequency and duration of the recorded tone and that no period was
repeated or skipped. `hda_pcm_poll` runs the same test without an
interrupt. `hda_audiod` runs the `audiod` integration test.

Document: `docs/design/hda.md`, `docs/design/audio.md`.

### S3. Opus in Ogg

A libcodec module that decodes Opus streams in Ogg (RFC 6716, RFC 7845)
with the decoder of libopus: SILK, CELT and the hybrid mode, the channel
mapping families 0 and 1, the pre-skip and the output gain. The existing
Ogg demuxer of libcodec supplies the packets. The module adds a probe, the
MIME type `audio/ogg; codecs=opus` with the extension `.opus`, and
seeking.

Boot test: `codec_opus` decodes reference files of the Opus test vectors
in all three modes and compares checksums of the output.

Document: `docs/design/codecs.md`.

### S4. AAC-LC in MP4 and ADTS

A libcodec module with a minios decoder for AAC-LC (ISO/IEC 14496-3):
the bitstream syntax of single channel, channel pair and fill elements,
section and scale factor data, Huffman decoding of spectral data, inverse
quantisation, M/S and intensity stereo, TNS, the long and short window
sequences and the IMDCT with overlap-add. Two containers supply the
frames: MP4 through the boxes `ftyp`, `moov`, `trak`, `mdia`, `minf`,
`stbl`, `stsd` (`mp4a` with `esds`), `stsz`, `stco`, `co64`, `stsc` and
`stts`, and the ADTS frames of `.aac` files. HE-AAC streams are decoded
at their AAC-LC core rate.

Boot test: `codec_aac` decodes reference files in both containers, mono
and stereo, and compares the output against reference PCM within a
tolerance.

Document: `docs/design/codecs.md`.

### S5. Notifications

`notifyd` runs in each graphical session and serves the protocol
`notify` over libwire. It shows each notification as a pop-up in the top
layer in the top right corner of the output, stacks several pop-ups,
closes them after their timeout unless they are critical, and reports
clicks on actions back to the sender. libgui provides `gui_notify` for
programs, and `notify-send` posts notifications from the shell. The panel
gets a notification button that lists the history of the session and
offers a do-not-disturb switch.

Boot tests: `gui_notify` posts notifications with `notify-send`, checks
the pop-up on the screen, invokes an action with the pointer and checks
the history in the panel. `notify_proto` checks the protocol with a test
client: replacement by identifier, closing, expiry and the
do-not-disturb state.

Document: `docs/design/notifications.md`.

### S6. Screen locker

X12 offers a session lock protocol. While the session is locked, X12
shows only the lock surface on every output, delivers all input to it, and
disables the global shortcuts. If the locker exits without unlocking, X12
shows a black screen with a message and accepts a new locker. The locker
`lock` verifies the password through the setuid helper `checkpass`, which
reads `/etc/shadow` and delays after failures. The session locks
on Super+L, from the power menu of the panel, and after a period of
inactivity that X12 measures and the settings application configures.

Boot tests: `gui_lock` locks the session, checks that a window below
does not receive keys, rejects a wrong password and unlocks with the
right one. `gui_lock_crash` kills the locker and checks that the session
stays locked. `lock_idle` checks the idle timeout.

Document: `docs/design/lock.md`, `docs/design/compositor.md`.

### S7. Workspaces and window snapping

X12 manages four workspaces. Super+1 to Super+4 and Ctrl+Alt+Left and
Right switch between them, and Super+Shift+1 to 4 move the active window.
The toplevel manager protocol reports the workspace of each window, and
the panel shows a workspace switcher. Windows snap to the left or right
half of the desktop when they are dragged to the edge, maximise when
dragged to the top, and respond to Super+Left, Super+Right and Super+Up.
X12 draws a preview of the target area during the drag.

Boot tests: `gui_workspaces` and `gui_snap`.

Document: `docs/design/compositor.md`.

### S8. Archive manager

The tar reader of `pkg` moves into libc as a shared ustar and pax module.
`archives` is a GUI application that opens `.tar`, `.tar.gz`, `.tgz` and
`.gz` files, lists their contents, extracts all or selected entries, and
creates archives from files dropped on it or chosen in a dialog. The gzip
compressor of libc gains dynamic Huffman blocks, and `gunzip` and `zcat`
are added.

Boot tests: `gui_archives` and `gzip_dynamic`.

Document: `docs/design/archives.md`.

### S9. PDF parser and renderer

`libpdf` parses the file structure (cross-reference tables and streams,
object streams, incremental updates), decodes the stream filters (Flate,
LZW, ASCIIHex, ASCII85, RunLength and DCT), and interprets content
streams: the graphics state, paths, clipping, text with TrueType, CFF and
Type 1 fonts and the standard 14 fonts, and images in the gray, RGB, CMYK
and indexed colour spaces. The path rasteriser of the SVG module becomes
a shared rasteriser with transforms, stroking and clipping, which libfont
also uses.

Boot test: `pdf_render` renders reference documents and compares them
against reference images.

Document: `docs/design/pdf.md`.

### S10. PDF viewer

`pdfview` shows a document page by page or as a continuous scroll, with
zoom, fit to width, the outline of the document and links within the
document. It is registered for `application/pdf`.

Boot test: `gui_pdfview`.

Document: `docs/design/pdf.md`.

### S11. Trash

Delete in Files moves items to the trash; Shift+Delete removes them
permanently after a confirmation. The sidebar lists the trash, which
offers restore and empty. The command `trash` puts files in the trash
from the shell.

Boot test: `gui_trash`.

Document: `docs/design/files.md`.

### S12. File search in Files

The existing name search of Files gains content search, filters for type,
size and modification date, and incremental results without the current
depth and count limits.

Boot test: `gui_files_search`.

Document: `docs/design/files.md`.
