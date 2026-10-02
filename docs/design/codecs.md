# Formats and codecs

This document describes `libcodec`, the library that minios programs use
to read and write image and audio files, and the format modules in
`/lib/codecs`. The plan is `docs/plan/codecs.md`.

## Model

The design follows the gdk-pixbuf loaders and the GStreamer plugins of
Linux. A program does not specify a format. It passes a file, or the
bytes of one, to the library. The library runs the probe of every codec
on the data and selects the codec with the highest score. Every format lives in a module, a shared object that the
library loads at run time. A new module therefore adds its format to
every program without rebuilding any of them. libgui, the applications
and the command line tools all use the same registry.

```
  view, paint, screenshot, desktop, player, codecs
          |                      |
        libgui (image_*)         |
          |                      |
        libcodec.so: registry, probing, files, inflate
          |  dlopen at the first lookup
        /lib/codecs/png.so  svg.so  ...
```

`libcodec.so` depends only on libc, and its header is `<codec/codec.h>`.
libgui links against it and keeps its own image functions as wrappers.
Programs and packages built against libgui need no change, and the ABI
number of libgui remains 1.

## Modules

A module is a shared object that exports exactly one symbol,
`codec_module`. This symbol is a `struct codec_module` that holds the
module ABI number (`CODEC_MODULE_ABI`, currently 1), a name, and a table
of `struct codec`. Modules are compiled with `-fvisibility=hidden`, and
the `CODEC_MODULE` macro gives the table default visibility. The helper
functions of different modules can therefore never resolve to each
other. A module links against `libcodec.so`, which provides shared
helpers such as `codec_inflate`, and against libc.

Each `struct codec` describes one format with these members:

- `name` and `description`, used by lookups and listings.
- `kind`, either `CODEC_IMAGE` or `CODEC_AUDIO`.
- `caps`, a combination of `CODEC_DECODE`, `CODEC_ENCODE` and
  `CODEC_SCALABLE`. The last one marks a vector format that renders at
  the size the caller requests, like `GDK_PIXBUF_FORMAT_SCALABLE`.
- `mime_types` and `extensions`, each a list separated by spaces.
- `probe`, which rates the first bytes of some data (at most
  `CODEC_PROBE_LEN`, 512 bytes) from 0 to 100, in the way a gdk-pixbuf
  loader matches its patterns.
- The functions for its kind: `image_decode` and `image_encode` for
  images, and `audio_open`, `audio_read`, `audio_close` and
  `audio_encode` for audio. A function for a capability the codec lacks
  is NULL.

The build turns every directory `libcodec/modules/NAME/` into
`build/lib/codecs/NAME.so`, which is installed as `/lib/codecs/NAME.so`.

## The registry

The first call that needs the registry builds it, once per process,
under `pthread_once`. It lists the `*.so` files in `/lib/codecs`, or in
the directory named by the environment variable `CODEC_PATH` (the
counterpart of `GST_PLUGIN_PATH`). It sorts the names, opens each file
with `dlopen` by path and `RTLD_NOW`, and resolves the symbol
`codec_module` with `dlsym`. A
module that lacks the symbol, has a different ABI number, or exceeds
the limits of 32 modules and 64 codecs is closed again, and a message is
printed on standard error. The other modules remain loaded for the life
of the process. After this step the tables are only read, and lookups
take no lock. `codec_register` adds a module that is not in the
directory. It takes `register_lock`, and a program must call it before
it starts threads that call the lookup functions.

Sorting the names makes the order of the codecs independent of the order
of the directory entries. The codec that is selected when two probes
return the same score is therefore also independent of that order. The registry loads all modules at the first lookup, which
for a GUI program happens when it renders its first icon. gdk-pixbuf
avoids this cost with a cache file that lists the formats and loads a
module only when its format is needed. The modules of minios are small,
and the registry has no such cache.

The lookup functions are:

- `codec_find(name)`, which ignores case.
- `codec_for_mime(kind, mime, caps)`.
- `codec_for_path(kind, path, caps)`, which uses the extension after the
  last dot of the last path component and ignores case.
- `codec_for_data(kind, data, len, caps)`, which returns the codec with
  the highest probe score above 0.
- `codec_identify(kind, data, len, path, caps)`, which calls
  `codec_for_data` and, when no probe returns a score above 0,
  `codec_for_path`. gdk-pixbuf uses the same order.

A `kind` of 0 matches both kinds, and `caps` lists the capabilities the
codec must have. For example, `codec_for_path(CODEC_IMAGE, "x.svg",
CODEC_ENCODE)` returns NULL even though an SVG decoder exists.

## Images

A picture (`struct codec_picture`) holds `w` by `h` pixels of
`0xAARRGGBB` with straight alpha in memory allocated with `malloc`. This
is the layout of libgui's `struct image`, and libgui uses the pixel
memory directly without copying it. A request (`struct codec_image_request`)
gives the size at which a vector format should render and the colour of
shapes that have no colour of their own. Raster formats ignore the
request.

`codec_image_decode(c, data, len, path, req, out)` decodes with `c`, or
with the codec that `codec_identify` selects when `c` is NULL. It returns
`-ENOTSUP` when no codec can decode the data. `codec_image_load` reads a
file and decodes it. `codec_image_encode(c, pic, &data)` returns an
encoded file in memory. `codec_image_save(pic, path, name)` encodes with
the codec called `name`, or with the codec for the extension of `path`
when `name` is NULL, and writes the file.

The libgui functions map onto these calls. `image_decode` and
`image_load` decode any format by content. `image_render_svg` and
`image_load_svg` call the decode function of the `svg` codec with a
request of px by px pixels.
`image_encode_png` and `image_save_png` use the `png` codec, and
`zlib_inflate` calls `codec_inflate`. As before, they return NULL and set
`errno` on failure.

The image modules of C1 are:

| Module | Codec | Capabilities | Probe |
|---|---|---|---|
| `png.so` | `png`, `image/png`, `.png` | decode, encode | the eight byte signature, 100 |
| `svg.so` | `svg`, `image/svg+xml`, `.svg` | decode, scalable | `<svg` after an optional byte order mark, XML declaration, comments and white space, 90 |

`images.md` describes the PNG decoder and encoder, and `icons.md` the SVG
renderer. The SVG codec renders a square whose side is the width of the
request, or its height when the width is 0, or 256 pixels when both are
0.

## Audio

Audio is decoded as a stream. `codec_audio_open(c, data, len, path, &a)`
selects the codec in the same way as an image decode and calls its
`audio_open`. That function checks the data and reports the format and
the number of frames, or -1 when the number is unknown. The format
(`struct codec_audio_format`) gives the frames per second, the number of
channels and the number of bits of a sample in the file.
`codec_audio_read(a, samples, frames)` returns up to `frames` frames of
interleaved signed 32 bit samples with full scale at 2^31. Every PCM
sample size fits into this representation without loss. At the end of
the stream the function returns 0. The data must remain valid until
`codec_audio_close`. `codec_audio_open_file` reads a file into memory,
stores the contents in the decoder, and `codec_audio_close` frees them. `codec_audio_encode` and
`codec_audio_save` take samples in the same representation, together with
the format to write.

The audio module of C2 is:

| Module | Codec | Capabilities | Probe |
|---|---|---|---|
| `wav.so` | `wav`, `audio/x-wav audio/wav audio/vnd.wave`, `.wav .wave` | decode, encode | `RIFF` and `WAVE`, 100 |

`wav.so` walks the RIFF chunks to find `fmt ` and `data` and skips the
pad byte after a chunk of odd length. It accepts the PCM format tag, and
the extensible tag when the first two bytes of its sub-format GUID
identify PCM. It accepts one to eight channels and samples of 8 bits
(unsigned) or 16, 24 and 32 bits (signed). A `data` chunk that claims to
extend beyond the end of the file is cut at the end of the file, and the
stream contains the complete frames that remain. Each sample is shifted
to the top of the 32 bit value, and an 8 bit sample has its sign bit
inverted first. The most negative sample of every size therefore
becomes -2^31. The encoder writes the PCM tag and a 44 byte header at the
sample size of the format, or 16 bits when the format gives 0. It keeps
the upper bits of each sample and pads data of odd length. Decoding a
file and encoding it again at its own sample size gives an identical
file.

`player` (`audio.md`) opens files with `codec_audio_open_file` and reads
them in chunks of 4096 frames. It keeps the upper 16 bits of each sample
for its resampler and no longer contains its own WAV reader. Its package
lists `libcodec.so` as a requirement, derived from its `DT_NEEDED`
entries.

## A new format: BMP

After the viewer and paint stopped naming formats (see below), adding
`bmp.so` required no change to any program. The viewer, paint, the
desktop wallpaper and the `codecs` command read BMP files as soon as the
module is installed in `/lib/codecs`.

| Module | Codec | Capabilities | Probe |
|---|---|---|---|
| `bmp.so` | `bmp`, `image/bmp image/x-bmp`, `.bmp .dib` | decode, encode | `BM` and a known header size, 90 |

The decoder reads the file header and a `BITMAPINFOHEADER` (40 bytes) or
one of its V2, V3, V4 and V5 extensions (52, 56, 108 and 124 bytes). It
accepts 24 bit pixels and 32 bit pixels. A 32 bit image is stored either
as `BI_RGB`, in which the fourth byte is unused and the image is opaque,
or as `BI_BITFIELDS`. For `BI_BITFIELDS` the decoder reads the masks from
the header in V2 and later, and from the bytes after a 40 byte header in
the original version. It reads an alpha mask only when the header or the
pixel offset leaves room for one. Each mask must be contiguous and at
most eight bits wide, and the decoder scales its channel to eight bits.
A negative height means that the rows are stored from the top down. Rows
are padded to a multiple of four bytes. The width and the height are
limited to 16384 pixels, and the pixel data must lie entirely within the
file.

The encoder writes a `BITMAPV4HEADER` with 32 bit `BI_BITFIELDS` pixels,
the masks of `0xAARRGGBB` and the sRGB colour space, with rows from the
bottom up. Alpha survives the file, and a picture that is encoded and
decoded again is unchanged.

## Programs

`view` lists the files of a directory whose extension a codec can decode
(`codec_for_path` with `CODEC_DECODE`). It decodes each file by content
with a request of 1024 by 1024 pixels, which only vector formats use. The
Set as wallpaper entry is enabled only for formats without
`CODEC_SCALABLE`. paint opens every decodable format. It saves in the
format that matches the extension of the file name, and in PNG when no
codec encodes that extension. The desktop loads its wallpaper with
`image_load`, which makes a BMP file a valid wallpaper as well.
`/etc/mime.types` lists `image/bmp` with `.bmp` and `.dib`,
`image/svg+xml` with `.svg`, and `.wave` next to `.wav`. Files uses these
entries to open such files with the viewer and the player.

## The codecs command

`/bin/codecs` (`user/coreutils/codecs.c`, `codecs(1)`) shows the
registry in the manner of `gst-inspect` and converts files in the manner
of `ffmpeg`.

- `codecs` prints one line per codec with its capabilities (`D`, `E`,
  `S`), its name, its kind, its module file and its description, then a
  second line with its MIME types and extensions, and finally the
  totals. A typical line is `DE- bmp   image  bmp.so   Windows bitmap`.
- `codecs info FILE...` prints the name of the codec that
  `codec_identify` selects and decodes the file. For an image it prints the size and states
  whether the image has alpha and whether it is scalable. For an audio
  stream it prints the rate, the channels, the sample size, the number
  of frames and the duration.
- `codecs convert [-f NAME] [-b BITS] [-s SIZE] IN OUT` decodes `IN` and
  encodes it with the codec called `NAME`, or with the encoder of the
  same kind that matches the extension of `OUT`. `-b` sets the sample
  size of converted audio and `-s` the size at which a vector image is
  rendered. On success it prints `IN (codec) -> OUT (codec)`.

The exit status is 0 on success, 1 when a file cannot be read,
identified, decoded, encoded or written, and 2 for incorrect usage.

## Files and errors

`codec_read_file` reads a whole file into memory, and `codec_write_file`
writes one. Every function that can fail returns a negative errno value.
`-ENOTSUP` means that no codec has the required capability. It was added
to libc's `errno.h` with the value of `EOPNOTSUPP`, as on Linux.
`-EINVAL` means that a codec rejected the data. File system errors are
passed through unchanged.

## Static programs and host tests

A static program has no dynamic loader. `dlopen` fails in such a
program, the registry stays empty, and the image functions return
`-ENOTSUP`. A static link against `libgui.a` must add `-lcodec`
(`tcc.md`).

The host unit tests compile libcodec and every module into the test
program with `-DCODEC_BUILTIN`. In this mode `CODEC_MODULE(name)` defines
a symbol called `codec_module_NAME`, and the registry registers the list
of built-in modules in `registry.c` instead of reading a directory. A new
module must be added to that list.

## The loader

Loading the modules one after another through a single path buffer
revealed a bug in `/lib/ld.so`. An object opened with `dlopen` kept a
pointer to the caller's string as its name. The next `dlopen` call with
the same buffer then compared the new path with itself and returned the
first module again. The loader now stores a copy of the name of every
object that `dlopen` opens (`dynlink.md`), and `dltest` checks a reused
buffer.

## Tests

`user/tests/codectest.c` lists the modules it finds and then runs the
checks selected by its argument.

With the argument `image`, used by the boot test `codec_image`, it
checks the lookups by name, MIME type and extension, the capability
filter, and checks that `codec_register` returns `-EINVAL` for a module
with another ABI number. It probes
PNG data, SVG data and plain text, and checks the fallback to the
extension. It encodes opaque, translucent and transparent pixels as PNG
and decodes them again, both in memory and through a file. It also
checks a truncated PNG file, saving to an unknown extension, saving to a
format without an encoder, a missing file, SVG rendering with and without
a request, the libgui wrappers, and a child process whose `CODEC_PATH`
names an empty directory.

With the argument `audio`, used by the boot test `codec_audio`, it looks
up the WAV codec by name, MIME type and extension. It encodes 301 frames
at every sample size with one to three channels, decodes them in chunks
of seven frames, and compares every sample. It checks the bytes of 8 bit
samples and the pad byte, probing, a header without chunks, a file cut
in the middle of a sample, a request for 12 bit samples, the extensible
format with PCM and with floating point samples, image data, and a file
saved by extension and opened again.

The boot test `codec_tool` runs `/etc/tests/codecs.sh`. The script checks
that the listing shows the four modules with their capabilities. It
converts an icon from PNG to BMP and back and verifies that the two BMP
files are identical byte for byte. It runs `info` on both files, on a BMP
file named `.png`, which must be identified by its content, and on a
generated SVG file. It converts with `-f` and renders an SVG file with
`-s`. It identifies the chime, converts it to 24 bits and back to 16, and
compares the result with a direct conversion. Finally it checks the
errors for unknown data, an extension without an encoder, formats of
different kinds, incorrect usage and a missing file.

`gui_images` converts the saved drawing to BMP with `codecs` and shows
the BMP file in the viewer. Its post script decodes the BMP file with its
own reader and compares the pixels with the PNG file. `make check` runs
the same codecs compiled into the host tests, and `audio_player` covers
the player.
