# Release 0.7.0: devices and desktop services

This is the plan for release 0.7.0 of `docs/plan/roadmap.md`. Its milestones
are numbered with the prefix `S`. Each milestone comes with boot tests and
adds or updates the design documents it names. A milestone is marked as
completed here once its boot tests pass. The owner settled the open scope
questions on 2026-10-06; section 2 records the decisions.

## 1. Motivation and scope

Releases 0.5 and 0.6 extended the range of machines that minios runs on and
improved its integration with the host. Two common virtual devices are still
unsupported, however: the Intel e1000e network card and Intel HD Audio.

The desktop also lacks several services that users expect. Programs have no
way to show notifications, the session cannot be locked, all windows share a
single desktop, and windows can only be arranged by hand. Some everyday
applications are missing as well: there is no archive manager and no PDF
viewer, deleted files are removed immediately, and the search in Files only
matches file names.

When this plan is complete:

- minios supports the e1000e network card and Intel HD Audio controllers.
- libcodec decodes Opus audio in Ogg files and AAC-LC audio in MP4 and ADTS
  files.
- Programs can post notifications. They appear as pop-ups and are listed
  in a history in the panel.
- The session can be locked on request and locks itself after a period of
  inactivity. Only the user's password unlocks it.
- The desktop offers several workspaces, and windows can be snapped to the
  screen edges and halves.
- An archive manager opens, extracts and creates tar and gzip archives.
- A PDF viewer displays PDF documents, including their embedded fonts and
  images.
- Files moves deleted items to a trash and can search file contents.

Because the release is large, the owner asked for it to be implemented in
two parts. The first part covers S1 to S6, and the second part, S7 to S12,
follows in a later session.

## 2. Decisions

- The AAC-LC decoder is written from scratch for minios. The available
  decoders are FAAD2, which is GPL-licensed, and fdk-aac, whose licence is
  incompatible with the GPL; either would restrict the licensing of
  libcodec. As section 6 of the roadmap requires, the patent situation is
  checked before the module becomes part of a release.
- The Opus decoder is a port of the reference implementation, libopus,
  which is BSD-licensed. Only the decoder is included.
- The PDF renderer is also written for minios. MuPDF is AGPL-licensed, and
  PDFium requires a C++ runtime, which minios does not have. The renderer
  covers what ordinary documents need (see S9). Encryption, forms,
  annotations other than links, JavaScript, transparency groups and
  shading types 4 to 7 are out of scope.
- Notifications are handled by a per-session daemon that speaks a libwire
  protocol. The data model follows the freedesktop notification
  specification: a summary, a body, an application name, an icon, an
  urgency level, a timeout and up to three actions.
- The screen locker follows the design of the Wayland `ext-session-lock`
  protocol. X12 itself ensures that the lock surface covers every output
  and receives all input. If the locker crashes, the session therefore
  remains locked instead of becoming visible.
- The trash follows the freedesktop Trash specification, with the
  directories `files/` and `info/` under `~/.local/share/Trash`.

## 3. Milestones

### S1. The e1000e network card (D6) (completed 2026-10-06)

A driver for the Intel 82574L, which QEMU emulates as `e1000e`. The driver
uses one receive ring and one transmit ring with legacy descriptors, reads
the MAC address from the EEPROM, tracks the link state and registers the
card with the network core under the next free name `ethN`. Interrupts are
delivered by MSI. Without MSI, or with the kernel option `e1000e=poll`, a
timer of the network worker polls the rings instead. As part of this
milestone, network drivers now register a service function with the worker,
which no longer calls the virtio-net driver by name.

Boot tests: `net_e1000e` exchanges 640 raw frames with the echo peer,
checks the MAC address and the link state, and resets the controller.
`net_e1000e_poll` runs the same test without interrupts. `net_e1000e_dhcp`
configures the interface by DHCP over QEMU's user-mode network.

Documents: `docs/design/e1000e.md`, `docs/design/network.md`.

### S2. Intel HD Audio (D7) (completed 2026-10-06)

A driver for Intel HD Audio controllers. It sends codec commands through the
CORB and RIRB rings, walks the codec's widget graph to find a path from a DAC
to an output pin, and plays audio through an output stream descriptor with a
buffer descriptor list. The driver registers a playback PCM device with the
kernel audio layer, so `audiod` can use it just like virtio-snd. Position
updates arrive through MSI, or from a polling thread when `hda=poll` is
given.

Boot tests: `hda_pcm` plays a test tone through `intel-hda` and
`hda-duplex` into QEMU's wav backend. Its post script checks the frequency
and duration of the recorded tone and verifies that no period was repeated
or skipped. `hda_pcm_poll` runs the same test without interrupts.
`hda_audiod` runs the `audiod` integration test.

Documents: `docs/design/hda.md`, `docs/design/audio.md`.

### S3. Opus in Ogg (completed 2026-10-06)

A libcodec module that decodes Opus streams in Ogg files (RFC 6716 and RFC
7845) with the libopus decoder. It supports the SILK, CELT and hybrid
modes, channel mapping families 0 and 1, the pre-skip and the output gain.
The existing Ogg demuxer of libcodec supplies the packets. The module
provides a probe function and registers the MIME type `audio/opus` and
the extension `.opus`.

Boot test: `codec_opus` decodes reference files in all three modes, a 5.1
file, a chained file and a file with an output gain. It compares the
output with the samples that ffmpeg decodes with libopus on the host.

Document: `docs/design/codecs.md`.

### S4. AAC-LC in MP4 and ADTS (completed 2026-10-06)

A libcodec module with a minios implementation of an AAC-LC decoder
(ISO/IEC 14496-3). The decoder covers the bitstream syntax of
single-channel, channel-pair and fill elements, section data and scale
factor data, Huffman decoding of the spectral data, inverse
quantisation, M/S and intensity stereo, TNS, the long and short window
sequences, and the IMDCT with overlap-add. Frames come from two
containers. MP4 files are read through the boxes `ftyp`, `moov`,
`trak`, `mdia`, `minf`, `stbl`, `stsd` (`mp4a` with `esds`), `stsz`,
`stco`, `co64`, `stsc` and `stts`. ADTS frames come from `.aac` files.
HE-AAC streams are decoded at the sample rate of their AAC-LC core.

Boot test: `codec_aac` decodes mono, stereo and 5.1 reference files in
both containers. The files include short windows, TNS, intensity stereo
and noise substitution. The test compares the output with the output of
FFmpeg's decoder. As the owner decided, `tools/gen_aac_tables.py`
extracts the tables of the standard from FFmpeg 7.1.

Document: `docs/design/codecs.md`.

### S5. Notifications (completed 2026-10-06)

`notifyd` runs in every graphical session and serves the `notify` protocol
over libwire. It owns the notifications, which includes their history, the
expiry of their pop-ups, the do-not-disturb state and the delivery of
actions to the programs that posted them. The panel is its display. It
shows each visible notification as a card in the top layer at the top
right corner of the desktop area and stacks several cards. It sends clicks
on the cards and their action buttons back to `notifyd`. Programs post
notifications with `gui_notify` or a `notify_client` from libgui, and
shell scripts use `notify-send`. A new bell button in the panel opens the
history of the session, together with a do-not-disturb switch.

The plan originally gave the drawing of the pop-ups to `notifyd`. The
panel draws them instead, because it already has the surfaces, the drawing
code and the pointer handling. As a result, `notifyd` can run without a
display.

Boot tests: `gui_notify` posts notifications with `notify-send` and checks
the pop-up on the screen. It invokes an action with the pointer, checks
expiry and the close button, opens the history and switches on
do-not-disturb. `notify_proto` checks the protocol with a test client. It
covers replacement by number, closing, expiry, actions, removal, the
do-not-disturb state and the history limit.

Document: `docs/design/notifications.md`.

### S6. Screen locker

X12 gains a session lock protocol. While the session is locked, X12 shows
only the lock surface on every output, sends all input to it and disables
the global shortcuts. If the locker exits without unlocking the session,
X12 shows a black screen with a message and accepts a new locker. The
locker program, `lock`, checks the password through the setuid helper
`checkpass`, which reads `/etc/shadow` and waits after each failed attempt.
The session can be locked with Super+L or from the power menu of the
panel, and it locks itself after a period of inactivity. X12 measures the
inactivity, and the timeout is set in the Settings application.

Boot tests: `gui_lock` locks the session, checks that a window underneath
receives no keys, rejects a wrong password and unlocks with the correct
one. `gui_lock_crash` kills the locker and checks that the session remains
locked. `lock_idle` checks the inactivity timeout.

Documents: `docs/design/lock.md`, `docs/design/compositor.md`.

### S7. Workspaces and window snapping

X12 provides four workspaces. Super+1 to Super+4 and Ctrl+Alt+Left/Right
switch between them, and Super+Shift+1 to Super+Shift+4 move the active
window to another workspace. The toplevel manager protocol reports the
workspace of each window, and the panel shows a workspace switcher. A
window dragged to the left or right screen edge fills that half of the
desktop, and a window dragged to the top edge is maximised. Super+Left,
Super+Right and Super+Up do the same from the keyboard. While a window is
being dragged, X12 shows a preview of the area it will occupy.

Boot tests: `gui_workspaces` and `gui_snap`.

Document: `docs/design/compositor.md`.

### S8. Archive manager

The tar reader of `pkg` moves into libc, where it becomes a shared module
for ustar and pax archives. `archives` is a new graphical application. It
opens `.tar`, `.tar.gz`, `.tgz` and `.gz` files, lists their contents and
extracts all entries or a selection. It also creates archives from files
that are dropped on its window or chosen in a dialog. The gzip compressor
in libc gains dynamic Huffman blocks, and the commands `gunzip` and `zcat`
are added.

Boot tests: `gui_archives` and `gzip_dynamic`.

Document: `docs/design/archives.md`.

### S9. PDF parser and renderer

`libpdf` reads the file structure of PDF documents: cross-reference tables
and streams, object streams and incremental updates. It decodes the Flate,
LZW, ASCIIHex, ASCII85, RunLength and DCT filters and interprets content
streams, including the graphics state, paths, clipping, text in TrueType,
CFF, Type 1 and the standard 14 fonts, and images in the gray, RGB, CMYK
and indexed colour spaces. The path rasteriser of the SVG module becomes a
shared rasteriser with support for transforms, stroking and clipping, and
libfont uses it as well.

Boot test: `pdf_render` renders reference documents and compares the
results with reference images.

Document: `docs/design/pdf.md`.

### S10. PDF viewer

`pdfview` displays a document either one page at a time or as a continuous
scroll. It supports zooming, fitting the page to the window width, the
document outline and links within the document. It is registered as the
handler for `application/pdf`.

Boot test: `gui_pdfview`.

Document: `docs/design/pdf.md`.

### S11. Trash

The Delete key in Files moves the selected items to the trash, and
Shift+Delete removes them permanently after a confirmation. The trash
appears in the sidebar, where items can be restored and the trash can be
emptied. A new command, `trash`, moves files to the trash from the shell.

Boot test: `gui_trash`.

Document: `docs/design/files.md`.

### S12. File search in Files

Files already searches by file name. This milestone adds searching inside
files, filters for file type, size and modification date, and results that
appear as they are found. It also removes the current limits on search
depth and on the number of results.

Boot test: `gui_files_search`.

Document: `docs/design/files.md`.
