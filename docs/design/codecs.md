# Formats and codecs

This document describes `libcodec`, the library through which minios
programs read and write image and audio files, and its format modules in
`/lib/codecs`. The plan is `docs/plan/codecs.md`.

## Model

The design follows the gdk-pixbuf loaders and the GStreamer plugins of
Linux. A program does not name a format: it hands the library a file or
the bytes of one, and the library finds the module that understands the
data. Every format is a module, a shared object that the library loads at
run time, so a new module makes every program read the new format without
being rebuilt. libgui, the programs and the command line tools all go
through the same registry.

```
  view, paint, screenshot, desktop, player, codecs
          |                      |
        libgui (image_*)         |
          |                      |
        libcodec.so: registry, probing, files, inflate
          |  dlopen at the first lookup
        /lib/codecs/png.so  svg.so  ...
```

`libcodec.so` depends only on libc. Its header is `<codec/codec.h>`.
libgui links it and keeps its own image functions as wrappers, so that
programs and packages built against libgui are unchanged and libgui keeps
ABI 1.

## Modules

A module is a shared object that exports exactly one symbol,
`codec_module`, a `struct codec_module` with the module ABI number
(`CODEC_MODULE_ABI`, 1), a name, and a table of `struct codec`. The
modules are compiled with `-fvisibility=hidden` and the `CODEC_MODULE`
macro gives the table default visibility, so the helper functions of two
modules never meet in a symbol lookup. A module links `libcodec.so` for
the shared helpers (`codec_inflate`) and libc.

Each `struct codec` describes one format:

- `name` and `description`, for lookups and listings;
- `kind`, `CODEC_IMAGE` or `CODEC_AUDIO`;
- `caps`, `CODEC_DECODE` and `CODEC_ENCODE`;
- `mime_types` and `extensions`, lists separated by spaces;
- `probe`, which scores the first bytes of some data (at most
  `CODEC_PROBE_LEN`, 512) from 0 to 100, as a gdk-pixbuf loader does
  with its patterns;
- the functions of its kind: `image_decode` and `image_encode` for
  images, `audio_open`, `audio_read`, `audio_close` and `audio_encode`
  for audio. A function of a capability the codec lacks is NULL.

The build turns every directory `libcodec/modules/NAME/` into
`build/lib/codecs/NAME.so`, installed as `/lib/codecs/NAME.so`.

## The registry

The registry is built once per process, by the first call that needs it,
under `pthread_once`. It lists the `*.so` files of `/lib/codecs`, or of
the directory in the environment variable `CODEC_PATH` (like
`GST_PLUGIN_PATH`), sorts their names, opens each with `dlopen` by path
and `RTLD_NOW`, and looks up `codec_module`. A module without the symbol,
with another ABI number, or beyond the limits of 32 modules and 64 codecs
is closed again with a message on standard error. The modules stay
loaded for the life of the process. Afterwards the tables are only read,
so lookups take no lock; `codec_register` adds a module that is not in
the directory and takes `register_lock`, which a program must do before
it starts threads that look codecs up.

Because the names are sorted, the order of the codecs, and with it the
winner of two equal probe scores, does not depend on the order of the
directory. All modules are loaded at the first lookup; a GUI program
opens them when it renders its first icon. gdk-pixbuf instead keeps a
cache file of the formats and loads a module only when its format is
needed; the modules of minios are small enough that the cache is not
needed yet.

Lookups:

- `codec_find(name)` without regard to case;
- `codec_for_mime(kind, mime, caps)`;
- `codec_for_path(kind, path, caps)` by the extension after the last dot
  of the last path component, without regard to case;
- `codec_for_data(kind, data, len, caps)`: the codec with the highest
  probe score above 0;
- `codec_identify(kind, data, len, path, caps)`: by content, and by the
  extension when no probe matches, the order of gdk-pixbuf.

`kind` 0 matches both kinds, and `caps` names the capabilities the codec
must have, so `codec_for_path(CODEC_IMAGE, "x.svg", CODEC_ENCODE)` finds
nothing although an SVG decoder exists.

## Images

A picture (`struct codec_picture`) is `w` by `h` pixels of `0xAARRGGBB`
with straight alpha in memory from `malloc`, the layout of libgui's
`struct image`, so libgui adopts the pixels without a copy. A request
(`struct codec_image_request`) gives the size at which vector formats
render and the fill of shapes without a colour of their own; raster
formats ignore it.

`codec_image_decode(c, data, len, path, req, out)` decodes with `c`, or
with the codec `codec_identify` picks; `-ENOTSUP` means that no codec
decodes the data. `codec_image_load` reads a file first.
`codec_image_encode(c, pic, &data)` returns a file in memory, and
`codec_image_save(pic, path, name)` encodes with the codec named `name`,
or by the extension of `path`, and writes the file.

libgui's functions map onto these: `image_decode` and `image_load` decode
any format by content, `image_render_svg` and `image_load_svg` ask the
`svg` codec for a px by px picture, `image_encode_png` and
`image_save_png` use the `png` codec, and `zlib_inflate` is
`codec_inflate`. They return NULL with `errno` set, as before.

The modules of C1:

| Module | Codec | Capabilities | Probe |
|---|---|---|---|
| `png.so` | `png`, `image/png`, `.png` | decode, encode | the eight byte signature, 100 |
| `svg.so` | `svg`, `image/svg+xml`, `.svg` | decode | `<svg` after an optional byte order mark, XML declaration, comments and white space, 90 |

The PNG decoder and encoder are described in `images.md`, the SVG
renderer in `icons.md`. The SVG codec renders a square of the request's
width, else its height, else 256 pixels.

## Audio

Audio is decoded as a stream. `codec_audio_open(c, data, len, path,
&a)` picks the codec like an image decode and calls its `audio_open`,
which checks the data and reports the format (`struct
codec_audio_format`: frames per second, channels, and the bits of a
sample in the file) and the number of frames, or -1 when it is not
known. `codec_audio_read(a, samples, frames)` returns up to `frames`
frames of interleaved signed 32 bit samples, full scale at 2^31, so that
every PCM sample size converts without loss; it returns 0 at the end.
The data must stay valid until `codec_audio_close`.
`codec_audio_open_file` reads a file and lets the decoder own its
contents. `codec_audio_encode` and `codec_audio_save` take samples in
the same form and the format to write.

The module of C2:

| Module | Codec | Capabilities | Probe |
|---|---|---|---|
| `wav.so` | `wav`, `audio/x-wav audio/wav audio/vnd.wave`, `.wav .wave` | decode, encode | `RIFF` and `WAVE`, 100 |

`wav.so` walks the RIFF chunks for `fmt ` and `data`, skipping the pad
byte of odd chunks. It accepts the PCM format tag, and the extensible
tag when the first two bytes of its sub-format GUID name PCM; one to
eight channels; and 8 bit unsigned or 16, 24 and 32 bit signed samples. A
`data` chunk longer than the file is cut to the file, and the frames are
the whole frames in it. Samples are moved to the top of the 32 bit value
(an 8 bit sample has its sign bit flipped first), so the most negative
sample of every size is -2^31. The encoder writes the PCM tag and a
44 byte header at the sample size of the format (16 when it is 0), keeps
the upper bits of each sample, and pads odd data. A file decoded and
encoded again at its own size is therefore unchanged.

`player` (`audio.md`) opens files with `codec_audio_open_file` and reads
them in chunks of 4096 frames into 16 bit samples, the upper half of each
value, before its resampling; it no longer contains a WAV reader. Its
package records `libcodec.so` among its needs, from its `DT_NEEDED`
entries.

## Files and errors

`codec_read_file` reads a whole file into memory and `codec_write_file`
writes one. Every function that can fail returns a negative errno value:
`-ENOTSUP` (added to libc's `errno.h` with the value of `EOPNOTSUPP`, as
on Linux) when no codec has the needed capability, `-EINVAL` for data a
codec rejects, and the errors of the file system.

## Static programs and host tests

A static program has no loader, so `dlopen` fails and the registry is
empty; its image functions return `-ENOTSUP`. A static link against
`libgui.a` names `-lcodec` as well (`tcc.md`).

The host unit tests compile libcodec and every module into the test
program with `-DCODEC_BUILTIN`. `CODEC_MODULE(name)` then defines
`codec_module_NAME`, and the registry registers the list of builtin
modules in `registry.c` instead of reading a directory; a new module is
added to that list.

## The loader

Opening the modules one after the other through one path buffer found a
bug in `/lib/ld.so`: an object opened with `dlopen` kept a pointer to the
caller's name, so the next `dlopen` with the same buffer compared its
name with itself and returned the first module again. The loader now
keeps a copy of the name of every object `dlopen` opens
(`dynlink.md`), and `dltest` checks a reused buffer.

## Tests

`user/tests/codectest.c` lists the modules it finds and runs the checks
of its argument. With `image`, the boot test `codec_image`, it checks
the lookups by name, MIME type and extension, the
capability filter, the refusal of a module of another ABI, probing of
PNG and SVG data and of text, the extension fallback, a PNG round trip
of opaque, translucent and transparent pixels in memory and through a
file, a truncated PNG, saving to an unknown extension and to a format
without an encoder, a missing file, SVG rendering with a request and
without one, libgui's wrappers, and a child process whose `CODEC_PATH`
names an empty directory. With `audio`, the boot test `codec_audio`, it
looks the WAV codec up by name, MIME type and extension, encodes and
decodes 301 frames at every sample size with one to three channels and
compares every sample, reading in chunks of seven frames, checks the
bytes of 8 bit samples and the pad byte, probing, a header without
chunks, a cut in the middle of a sample, a 12 bit request, the
extensible format with PCM and with float, image data, and a file saved
by extension and opened again. `make check` covers the same codecs built
in, and `audio_player` the player.
