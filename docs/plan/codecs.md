# Formats and codecs

This plan moves the handling of file formats out of the programs and
libgui into a codec library with loadable format modules, in the manner
of the gdk-pixbuf loaders and the GStreamer plugins of Linux. Milestone
identifiers use the prefix `C`. Each milestone ends with a boot test,
updates `docs/design/codecs.md`, and is marked completed here when its
boot tests pass. The work is on the branch `bleeding-edge-codecs`.

## 1. Motivation and scope

Each format is handled where it was first needed. libgui decodes and
encodes PNG and renders SVG, `view` chooses between the two by the file
extension, and `player` contains its own WAV reader. A program that
should read another format has to be changed, and a new format has to be
added to every program that might meet it.

On Linux a program asks a library for an image or a stream, and the
library finds the module that handles the data: gdk-pixbuf loads a
loader module for the format from its module directory, GStreamer finds
a decoder plugin by the capabilities of the stream, and both pick a
module by the magic bytes of the data before the MIME type or the file
name. A new module makes every program read the new format.

The scope is images and audio. Compression (gzip) stays in libc.

## 2. Fixed decisions

- A new shared library `libcodec.so` holds the registry, the probing and
  the high level functions. It depends only on libc, so that libgui,
  libaudio users and command line tools can all use it. Its header is
  `<codec/codec.h>`.
- Every format is a module, a shared object in `/lib/codecs/`, which
  exports one `struct codec_module` named `codec_module` with an ABI
  number and a table of `struct codec` entries. A module may hold several
  codecs.
- The registry is built once per process, on the first call that needs
  it, under `pthread_once`: it opens every `*.so` of `/lib/codecs` with
  `dlopen` by path, checks the ABI number and keeps the module open. The
  registry is read only afterwards. A module that fails to load is
  skipped with a message on standard error.
- A codec describes itself by a name, a description, its kind (image or
  audio), its MIME types and file extensions, its capabilities (decode,
  encode), and a probe function that scores the first bytes of the data
  from 0 to 100, as the gdk-pixbuf loaders do.
- Data is chosen by content first: the codec of the kind with the highest
  probe score wins. Without a match the extension of the path decides,
  as with gdk-pixbuf. Encoding chooses by an explicit codec name, else by
  the extension of the target path.
- Images are 32 bit 0xAARRGGBB pixels with straight alpha, the layout of
  `struct image` of libgui. A decode request may give a size, which
  vector formats render at and raster formats ignore, and a fill colour
  for SVG icons.
- Audio is decoded as a stream: opening a decoder reports the sample
  rate, the channel count and the bits per sample of the source, and
  reads return interleaved signed 32 bit samples, full scale at
  2^31, so that 8, 16, 24 and 32 bit PCM convert without loss. Encoders
  take the same samples and the format to write.
- Errors are negative errno values, as everywhere in minios.
- The functions of libgui (`image_load`, `image_decode`,
  `image_load_svg`, `image_render_svg`, `image_encode_png`,
  `image_save_png`) stay with their signatures and behaviour and call
  libcodec, so programs and packages built against libgui keep working
  and the ABI number of libgui does not change.
- The host unit tests (`make check`) compile the modules into the test
  program with `CODEC_BUILTIN`, which registers them without `dlopen`.

## 3. Milestones

### C1: the codec library, PNG and SVG modules (completed 2026-10-03)

- `libcodec/` with the registry, the module loader, probing, lookups by
  name, MIME type, extension and data, `codec_image_load`,
  `codec_image_decode` and `codec_image_save`, and the audio stream
  functions that C2's module implements.
- The PNG decoder, the PNG encoder and zlib inflate move from libgui into
  the module `png.so`, the SVG renderer into `svg.so`.
- libgui's image functions call libcodec. `zlib_inflate` stays exported
  by libgui as a wrapper.
- The build installs `libcodec.so`, its ABI entry and the modules.
- Boot test `codec_image`: a test program lists the registry, probes PNG
  and SVG data and data of no known format, decodes and re-encodes a PNG
  file, renders an SVG icon, and checks the errors. `gui_images` and
  `icons` must still pass.
- Found on the way: `dlopen` kept the caller's name pointer, so a reused
  path buffer returned the previous module. The loader copies the name,
  and `dltest` checks it.

### C2: audio codecs and the WAV module (completed 2026-10-03)

- The module `wav.so` decodes PCM WAV files with 8, 16, 24 and 32 bit
  samples and up to eight channels, and encodes the same sample sizes.
- `player` decodes through libcodec and keeps its resampling.
- Boot test `codec_audio`: the test program writes WAV files of each
  sample size through the encoder, decodes them back sample for sample,
  and checks truncated and foreign data. `audio_player` must still pass.

### C3: a new format without program changes, the codecs tool (completed 2026-10-03)

- The module `bmp.so` decodes BMP files with 24 and 32 bit pixels,
  bottom up and top down, and encodes 32 bit BMP files with alpha.
- `view` lists and opens every file a module can decode, and paint saves
  in the format of the extension. Neither names a format any more.
- `/etc/mime.types` lists `image/bmp` and `image/svg+xml`.
- The command `codecs` lists the modules and codecs with their
  capabilities (`codecs`), identifies a file (`codecs info FILE`), and
  converts between formats of one kind (`codecs convert IN OUT`), in the
  manner of `gst-inspect` and `ffmpeg`.
- Boot test `codec_tool`: `codecs` lists the four modules, converts a PNG
  file to BMP and back with identical pixels, and identifies a WAV file,
  `gui_images` converts its drawing to BMP and shows it in the viewer.
- Codecs gained `CODEC_SCALABLE` for vector formats, which the viewer
  uses for its wallpaper entry.
- Found on the way: the shell ran a line ending in a backslash and
  newline without the next line. The lexer now reports such input as
  incomplete.

### C4: FLAC (completed 2026-10-03)

- The module `flac.so` decodes native FLAC streams. It reads the
  metadata blocks, requires STREAMINFO, skips the other blocks, and
  accepts an ID3v2 tag before the stream. It decodes fixed and variable
  block sizes, every sample rate and sample size code, one to eight
  channels with the three stereo decorrelation modes, CONSTANT,
  VERBATIM, FIXED and LPC subframes with wasted bits, and both Rice
  coding methods with escaped partitions. It checks the CRC-8 of every
  frame header and the CRC-16 of every frame, and it compares the MD5 sum
  in STREAMINFO with the decoded samples at the end of the stream, as
  `flac -t` does.
- The encoder writes CONSTANT, VERBATIM, FIXED and LPC subframes. LPC
  coefficients come from the autocorrelation of the windowed block and
  the Levinson-Durbin recursion, with orders up to 32 and a search of the
  order and the coefficient precision. Each frame uses the stereo mode
  with the smallest output, Rice partitions of the best order with the
  best parameter in each partition, and escaped partitions where they are
  smaller. STREAMINFO carries the frame size limits and the MD5 sum.
- libcodec provides MD5 to the modules.
- `tools/gen_codec_fixtures.py` creates fixtures on the host with the
  reference `flac` encoder (including LPC order 32, odd block sizes and
  12 and 20 bit samples) and with ffmpeg's FLAC encoder. A stream with
  variable block sizes is assembled from the frames of two encodes. The
  MD5 sums in the files verify the decoder.
- Boot test `codec_flac`: every fixture decodes with a matching MD5 sum,
  the encoder's files decode to identical samples at 8, 12, 16, 20, 24
  and 32 bits and one to eight channels, the reference `flac` tool
  accepts the encoder's files (checked by the post script on the host),
  and damaged headers, frames and MD5 sums are reported as errors.

### C5: Ogg and Vorbis decoding (completed 2026-10-03)

- The module `ogg.so` reads Ogg pages, checks their CRC-32, follows the
  logical streams of a file (multiplexed and chained), and assembles
  packets. It selects the codec of each logical stream by its first
  packet. The Ogg layer is written once and serves Vorbis, FLAC and Opus.
- The Vorbis decoder implements Vorbis I completely: the identification,
  comment and setup headers, codebooks with both lookup types, floor 0
  and floor 1, residues 0, 1 and 2, mappings with submaps and channel
  coupling, short and long windows, the inverse MDCT through an FFT,
  overlap and add, and the trimming of the first and last samples by the
  granule positions. Channels are returned in the order of WAV files.
- Fixtures made with libvorbis through `tools/codecref/vorbisref.c`
  (mono, stereo with short blocks, 5.1, three channels, low and high
  quality, chained streams, and a stream multiplexed with Ogg FLAC), with
  the decoding by libvorbisfile stored as FLAC files for comparison.
  Floor 0 decoding is verified in C6 with files from the encoder of
  minios, because libvorbis no longer writes floor 0.
- Boot test `codec_vorbis`: every fixture decodes to the same number of
  frames as the decoding by libvorbis and within one step of a 16 bit
  sample of it, and damaged pages and cut files are reported. `make check` compares the inverse
  MDCT with the direct formula.

### C6: Vorbis encoding (completed 2026-10-03)

- Encoders take options. `codec_audio_encode_options` and
  `codec_audio_save_options` pass a string of `name=value` pairs
  separated by commas, such as `quality=0.6`, to a new member
  `audio_encode_options` of `struct codec`. The member is appended to the
  structure, which changes the module ABI to 2. libcodec keeps ABI 1,
  because the existing members keep their offsets and programs only gain
  functions. `codecs convert` passes options with `-o`.
- libcodec gains an Ogg writer that packs packets into pages with
  lacing, continuation, granule positions, the flags of the first and the
  last page, and the CRC-32.
- The encoder chooses between blocks of 256 and 2048 samples by transient
  detection, applies the window of the decoder and a forward MDCT through
  the same DCT-IV, and computes per block a floor 1 curve from the
  spectral envelope minus a signal-to-noise ratio set by the quality
  (-0.1 to 1.0, 0.4 by default). The residue is the spectrum divided by
  the rendered floor and rounded to integers. Stereo uses square polar
  coupling and residue type 2, other channel counts residue type 1. The
  residue partitions fall into five classes, from silent partitions to
  values beyond 15, coded by vector books over integer grids, the largest
  class in two cascade passes.
- The encoder quantises the whole input first, counts the symbols of
  every codebook, builds Huffman codes from the counts and writes them
  into the setup header, then writes the packets. The first block is
  centred on the first sample and the granule position of the last page
  equals the length of the input, and no samples are trimmed or added.
- With the option `floor=0` the encoder writes floor 0 instead: it fits
  an LPC filter to the floor curve on the Bark scale, converts it to line
  spectral pairs and codes their differences with a scalar book. These
  files verify the floor 0 decoder against libvorbis, which no longer
  writes floor 0 itself.
- Boot test `codec_vorbis_enc`: files encoded at several qualities, with
  floor 0, in mono, stereo and 5.1, decode in minios with the length of
  the input and a signal-to-noise ratio above a limit for each quality.
  The post script decodes the same files with libvorbis on the host and
  requires its result to match the decoder of minios.
- Changed during the work: the masking estimate lowers the ratio for
  noise-like bands, applies the absolute threshold of hearing, and lets
  both allowances vanish at quality 1.0. A sixth residue class codes
  values up to 8190, because a floor 0 curve cannot follow narrow
  spectral lines without large residues.

### C7: FLAC in Ogg

- Decoding and encoding of the Ogg mapping of FLAC: the `\x7fFLAC`
  identification packet with STREAMINFO, the metadata packets, and one
  frame per packet, with granule positions in samples.
- Boot test `codec_oggflac`: ffmpeg's Ogg FLAC fixtures decode with
  matching MD5 sums, and files from the encoder decode in minios and with
  ffmpeg on the host.

### C8: Opus decoding

- The Opus decoder implements RFC 6716 with the updates of RFC 8251: the
  range decoder, the TOC byte and the frame packing modes, SILK (LPC,
  long-term prediction, stereo prediction, the excitation decoder and the
  resamplers), CELT (energy envelopes, the pyramid vector quantiser,
  band folding, anti-collapse, the post-filter and the inverse MDCT),
  hybrid mode, the redundancy frames for mode transitions, and packet
  loss concealment. The Ogg mapping of RFC 7845 supplies the header, the
  pre-skip, the output gain and the channel mapping families 0, 1 and
  255. Output is at 48 kHz.
- Fixtures made by ffmpeg with libopus in SILK, CELT and hybrid modes at
  several bit rates and channel layouts, with libopus' decoding stored as
  FLAC files for comparison.
- Boot test `codec_opus`: every fixture decodes to the same length as
  libopus and within the accuracy that the `opus_compare` tool of the
  RFC accepts.

### C9: Opus encoding

- The Opus encoder writes CELT frames with energy quantisation, the
  pyramid vector quantiser and band allocation, SILK frames with LPC and
  pitch analysis and excitation quantisation, and chooses between the
  modes by bit rate and signal. It writes Ogg Opus files with the header,
  the comment header and correct granule positions.
- Boot test `codec_opus_enc`: encoded files decode in minios and with
  ffmpeg and libopus on the host at the expected quality for each bit
  rate.
