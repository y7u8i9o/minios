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
- libgui's image functions call libcodec; `zlib_inflate` stays exported
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

### C3: a new format without program changes, the codecs tool

- The module `bmp.so` decodes BMP files with 24 and 32 bit pixels,
  bottom up and top down, and encodes 32 bit BMP files with alpha.
- `view` lists and opens every file a module can decode, and paint saves
  in the format of the extension; neither names a format any more.
- `/etc/mime.types` lists `image/bmp` and `image/svg+xml`.
- The command `codecs` lists the modules and codecs with their
  capabilities (`codecs`), identifies a file (`codecs info FILE`), and
  converts between formats of one kind (`codecs convert IN OUT`), in the
  manner of `gst-inspect` and `ffmpeg`.
- Boot test `codec_tool`: `codecs` lists the four modules, converts a PNG
  file to BMP and back with identical pixels, identifies a WAV file, and
  the viewer shows a BMP file.
