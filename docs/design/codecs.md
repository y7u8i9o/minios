# Formats and codecs

This document describes `libcodec`, the library that minios programs use
to read and write image and audio files, and the format modules in
`/usr/lib/codecs`. The plan is `docs/plan/codecs.md`.

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
        /usr/lib/codecs/png.so  svg.so  ...
```

`libcodec.so` depends only on libc, and its header is `<codec/codec.h>`.
libgui links against it and retains its own image functions as wrappers.
Programs and packages built against libgui need no change, and the ABI
number of libgui remains 1.

## Modules

A module is a shared object that exports exactly one symbol,
`codec_module`. This symbol is a `struct codec_module` containing the module
ABI number (`CODEC_MODULE_ABI`), a name, and a table of `struct codec`.
The number is 2 since C6 appended `audio_encode_options` to `struct
codec`, which changed the size of the entries in the table. Modules are compiled with `-fvisibility=hidden`, and
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
  images, and `audio_open`, `audio_read`, `audio_close`, `audio_encode`
  and `audio_encode_options` for audio. A function for a capability the
  codec lacks is NULL, and a codec without encoder options leaves
  `audio_encode_options` NULL.

The build turns every directory `lib/libcodec/modules/NAME/` into
`build/lib/codecs/NAME.so`, which is installed as `/usr/lib/codecs/NAME.so`.

## The registry

The first call that needs the registry builds it, once per process,
under `pthread_once`. It lists the `*.so` files in `/usr/lib/codecs`, or in
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

A picture (`struct codec_picture`) contains `w` by `h` pixels of
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

### Encoder options

`codec_audio_encode_options` and `codec_audio_save_options` pass a string
of `name=value` pairs separated by commas to the encoder, for example
`quality=0.6,floor=0`, the way the `-q` and codec options of ffmpeg
reach an encoder. The functions return `-EINVAL` for an option string
that is not empty when the codec takes no options, and an encoder
returns the same for an option it does not know or a value outside its
range. Modules parse the string with
`codec_option`, which copies the value of a named option, and
`codec_options_check`, which compares the names with a list of known
ones. The extension of `struct codec` retains the offsets of all existing
members, and libcodec therefore retains ABI 1 for programs.

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
sample size of the format, or 16 bits when the format gives 0. It retains
the upper bits of each sample and pads data of odd length. Decoding a
file and encoding it again at its own sample size gives an identical
file.

`player` (`audio.md`) opens files with `codec_audio_open_file` and reads
them in chunks of 4096 frames. It retains the upper 16 bits of each sample
for its resampler and no longer contains its own WAV reader. Its package
lists `libcodec.so` as a requirement, derived from its `DT_NEEDED`
entries.

## FLAC

C4 added `flac.so`, which reads and writes native FLAC streams as
specified in RFC 9639.

| Module | Codec | Capabilities | Probe |
|---|---|---|---|
| `flac.so` | `flac`, `audio/flac audio/x-flac`, `.flac` | decode, encode | `fLaC`, also after an ID3v2 tag within the probed bytes, 100 |

The module consists of `common.c` with the bit reader and writer, the
CRC-8 and CRC-16 of the format, the code tables and the STREAMINFO
parser, `decode.c`, `encode.c` and `module.c`. Both directions process
samples as 64 bit integers, because the side channel of a 32 bit stereo
stream needs 33 bits and an LPC prediction sum of such samples needs
more than 32.

### Decoding

`flac_open` skips an ID3v2 tag in front of the stream and requires the
`fLaC` marker followed by STREAMINFO as the first metadata block. It skips
all other metadata blocks and rejects the reserved block type 127. The
format reported to the caller is the sample rate, the channel count and
the sample size from STREAMINFO, and the number of frames is the total
sample count, or -1 when STREAMINFO records 0.

`flac_read` decodes one frame at a time into 64 bit channel buffers,
which grow with the largest block size met, and copies the interleaved
samples out with every sample shifted to the top of a 32 bit value. The
frame header parser accepts both blocking strategies, the frame or
sample number coded like UTF-8 in up to seven bytes, every block size
code including the 8 and 16 bit fields, every sample rate code including
the values in kHz, Hz and tens of Hz, the channel assignments for one to
eight independent channels and the left/side, side/right and mid/side
pairs, and the sample size codes for 8, 12, 16, 20, 24 and 32 bits. A
header must match its CRC-8, and its channel count and sample size must
match STREAMINFO.

A subframe is CONSTANT, VERBATIM, FIXED of order 0 to 4, or LPC of order
1 to 32 with a coefficient precision of up to 15 bits and a shift from 0
to 15, and it may declare wasted bits, which the decoder restores by a
left shift at the end. The residual uses Rice coding with 4 or 5 bit
parameters and partition orders up to 15, and an escaped partition
stores raw signed values of the given width. The decoder rejects
reserved subframe types and coding methods, partitions that do not
divide the block or leave the first partition with fewer samples than
the predictor order, Rice quotients that would exceed 32 bits, and a
frame whose CRC-16 does not match. It then undoes the stereo
decorrelation and adds the samples of the frame to an MD5 sum computed
over the layout defined by FLAC, which is each sample as a signed little
endian integer of `(bits + 7) / 8` bytes, interleaved by channel.

When no further frame header follows, the stream has ended. Bytes after
the last frame that contain no valid header, such as an ID3v1 tag, are
ignored. If a valid header appears later in those bytes, the stream is
damaged. At the end the decoder compares the number of decoded samples
with the total in STREAMINFO and its MD5 sum with the recorded one,
unless STREAMINFO records 0 for either. A damaged frame, a missing
frame, a wrong sample count and a wrong MD5 sum make the read that
encounters them return `-EBADMSG`, after the frames decoded before the
damage. `EBADMSG` was added to libc with the Linux value 74.

### Encoding

`flac_encode` accepts sample sizes from 4 to 32 bits (16 when the format
gives 0), one to eight channels and rates below 2^20. It cuts the input
into blocks of 4096 samples and writes each block as one frame with a
fixed block size. For every channel signal the encoder determines the
wasted bits common to all samples of the block and then compares the
exact size of a CONSTANT subframe for a constant block, a VERBATIM
subframe, the FIXED subframe whose order gives the smallest sum of
absolute residuals, and LPC subframes.

The LPC candidates come from the samples under a Tukey window with half
of the block in its cosine tapers. The encoder computes the
autocorrelation up to lag 32 and runs the Levinson-Durbin recursion,
which yields the coefficients and the prediction error of every order up
to 32. The three orders with the smallest size estimated from their
prediction errors are coded exactly at coefficient precisions of 12 and
15 bits. Quantisation carries the rounding error of each coefficient into
the next one and uses the largest shift from 0 to 15 at which the largest
coefficient still fits.

The residual of each candidate is coded for every partition order up to
8 that divides the block, with both Rice methods. In every partition the
encoder evaluates the parameter closest to the logarithm of the mean of
the folded residuals and its two neighbours, and it uses an escaped
partition with raw values whenever that takes fewer bits. A candidate
whose residual exceeds 32 bits is discarded. For a stereo frame the
encoder codes the left, right, side and mid signals and writes the
cheapest of the four channel assignments. STREAMINFO records the block
size, the smallest and the largest frame, the sample count and the MD5
sum of the input.

On the chime the encoder writes 15148 bytes against 18317 bytes from
`flac -8`, and on four seconds of synthesised stereo music it writes
394695 bytes against 395100.

## Ogg

The Ogg container (RFC 3533) carries Vorbis, Opus and FLAC. Its reader
lives in libcodec (`src/ogg.c`) and is shared by the modules of these
codecs, in the way libogg serves the Ogg codecs on Linux. A page begins
with `OggS`, the version 0, the flags for a continued packet, the first
page and the last page of a logical stream, the granule position, the
serial number of the stream, the page number, a CRC-32 over the page
with its CRC field set to zero, and the lacing values, whose sum is the
length of the body. A packet consists of lacing values of 255 ended by
one below 255 and may continue on the following pages of its stream.

`codec_ogg_reader_init` takes the file in memory and an `accept`
function supplied by the codec, which examines the first packet of a
logical stream. The reader follows the first stream that `accept`
accepts and skips the pages of all other streams, which is how a
decoder finds its stream in a multiplexed file. When the followed
stream ends, the reader moves on to the next stream accepted by
`accept` and in this way decodes chained files. `codec_ogg_next`
returns the packets of the followed streams in order, together with the
granule position of the page for the last packet ending on it, and
flags for the first and the last packet of a stream. A page that is cut
or fails its CRC, a packet left unfinished by the next page, and a file
that ends before the last page of the followed stream are reported as
`-EBADMSG`. Bytes after the last page that contain no page header, such
as a tag, end the file normally. `codec_ogg_first_packet` finds the
first packet of the first accepted stream among the first pages of a
file and serves the probes, and `codec_ogg_total_granule` adds up the
last granule positions of all accepted chained streams, which gives the
length of a file in samples.

The writer (`struct codec_ogg_writer`) builds the pages of one logical
stream in memory. `codec_ogg_write_packet` appends the lacing values and
the bytes of a packet to the current page, starts a new page with the
continuation flag when a packet needs more than the 255 lacing values of
a page, and starts a new page before a packet once the current one contains
4096 bytes. A page carries the granule position of the last packet that
ends on it, or -1 when none does. `codec_ogg_flush` ends the current page,
with the flag for the first page on the first one and the flag for the
last page when its argument asks for it, and computes the CRC.

## Vorbis

C5 added `vorbis.so` with a decoder for Vorbis I in Ogg, and C6 added its
encoder.

| Module | Codec | Capabilities | Probe |
|---|---|---|---|
| `vorbis.so` | `vorbis`, `audio/ogg audio/vorbis application/ogg`, `.ogg .oga` | decode, encode | an Ogg stream whose first packet starts with `\x01vorbis`, 100 |

`setup.c` reads the identification header with the channel count, the
sample rate and the two block sizes, checks the comment header, and
parses the setup header. A codebook stores its codeword lengths, either
ordered or unordered and sparse, and assigns the codewords in the order
of the entries, giving each entry the lowest free codeword of its
length as the specification requires. The codewords are inserted into a
binary tree, which the decoder walks one bit at a time. A codebook with
lookup type 1 or 2 also contains the vector of every entry, computed from
the packed float minimum and delta, the multiplicands and the sequence
flag. The setup header then supplies the floors, the residues, the
mappings with their submaps and coupling steps, and the modes.

For an audio packet, `decode.c` reads the mode and, for a long block,
the window flags of the neighbouring blocks. It decodes the floor of
every channel. The coupling steps mark a channel for residue decoding
when either of its partners has a floor in use, and the residues are
decoded per submap, where type 2 interleaves all channels of the submap
into one vector. The decoder then undoes the square polar coupling in
reverse order, multiplies each channel by its floor curve, and sets the
channels whose floor is unused to zero. The inverse MDCT and the window
follow. The end of a packet inside a floor makes that floor unused, and
inside a residue it ends the residue decode with the values read up to
that point, as the specification prescribes.

Floor 1 (`floor.c`) reads the two end points and the partition values
with their class and subclass books, predicts every further point from
its neighbours, and draws the curve as integer lines in the decibel
domain. The table of the specification for the conversion to amplitude
runs geometrically from 1.0649863e-07 to 1.0 in 256 steps, and the
decoder computes it instead of storing it. Floor 0 reads an amplitude,
a book number and the LSP coefficients as vectors, and renders the
filter curve at the positions of a Bark map, which is computed at setup
for both block sizes.

`mdct.c` computes the inverse MDCT of a block of size N through a DCT-IV
of length N/2, which is a complex FFT of length N/4 between a rotation of
the coefficient pairs and a rotation of the result. The IMDCT reads the
DCT-IV forwards and backwards with changed signs. The window of a block
rises and falls with the curve sin(pi/2 sin^2(...)) of the
specification, and a long block next to a short one uses the short
slope on that side, centred on its quarter point. The output of a
packet is the overlap of the previous block, from its centre, with the
current block, up to its centre, which is N_prev/4 + N/4 samples. The
first packet of a stream produces no output.

The granule positions trim the start and the end of every stream. The
decoder retains the output of a stream until the first page with a granule
position. If that position is smaller than the number of samples decoded
up to it, the difference is removed from the start of the stream. If the
last page of a stream has a position smaller than the decoded samples,
the difference is removed from its end. Chained streams with the rate and
the channel count of the first stream are decoded one after another. A
chained stream of a different format ends the file, because the format
of a decoder cannot change. The number of frames reported at open time
is the sum of the last granule positions of the chained streams.

The decoder converts the float samples to 32 bit integers with rounding
and clipping, and it returns the channels in the order of WAV files
instead of the Vorbis order. Vorbis has no sample size, and the format
reports 0 bits. `codecs info` omits the sample size in that case, the
player shows none, and conversions to WAV or FLAC write 16 bits.

### Fixtures and comparison

`tools/codecref/vorbisref.c` is compiled by the fixture generator
against the libvorbis of the host. It encodes WAV files with
libvorbisenc and decodes Ogg files with libvorbisfile into 16 bit
samples, rounded and reordered to WAV order. The generator encodes the
chime (`/usr/share/sounds/chime.ogg`) and test signals in stereo with
transients, which produce short blocks, in 5.1, in three channels, at a
low and at the highest quality, as two chained streams, and as a stream
multiplexed with an Ogg FLAC stream. The reference decodings are stored
as FLAC files. libvorbisfile 1.3.7 starts reading a chained file at its
last stream, and `vorbisref` therefore seeks to the first sample before
it reads.

On all eight fixtures the decoder returns the same number of frames as
libvorbis. Rounded to 16 bits, its samples differ from the reference by
at most one step, with an RMS difference between 0.02 and 0.31 steps,
which comes from the rounding of the float arithmetic.

### Encoding

`encode.c` encodes in two passes over the whole input. The first pass
plans the blocks, computes the floor and the quantised residue of every
block and channel, and counts the symbols of every codebook. Huffman
codes are then built from those counts, and the second pass writes the
three headers and one packet per block into Ogg pages. The options are
`quality`, from -0.1 to 1.0 with 0.4 as the default, and `floor`, 1 by
default or 0.

The blocks have 256 or 2048 samples. The first block is long and centred
on the first sample, and the centres of neighbouring blocks lie a
quarter of each block apart, as the decoder overlaps them. A block is
short when the energy of 64 samples of the high-passed mix of the
channels rises more than eightfold over the mean of the preceding 512
samples anywhere within the span of a long block around its centre. The
blocks continue until a centre lies at or beyond the end of the input.
The granule position of a packet is the centre of its block, limited to
the length of the input, which makes the decoder return exactly the
input length without trimming at the start.

Each block is windowed with the window of the decoder (`vb_window`),
transformed by the forward MDCT (`vb_mdct` in `mdct.c`, which folds the
samples into the DCT-IV used by the inverse transform) and scaled by
4/N, the factor at which the overlap and add of the decoder restores the
input. The floor is a masking estimate. For a band around each floor
position, the largest magnitude is lowered by a signal-to-noise ratio of
12 + 36 * quality dB. A band whose power is spectrally flat, which marks
noise, needs up to 18 dB less, and above 8 kHz the ratio falls by up to
6 dB. Both allowances shrink with rising quality and vanish at 1.0. The
estimate never falls below the absolute threshold of hearing in
Terhardt's approximation, with full scale taken as 96 dB SPL, lowered by
up to 30 dB at the highest quality and capped at 90 - 70 * quality dB
SPL.

Floor 1 has 16 positions for short blocks and 60 for long ones, spaced
geometrically and coded in an order that halves the intervals, which
retains the predictions of the decoder close. The encoder computes the
value for each position that makes the decoder's prediction rule produce
the desired point, and renders the floor with the decoder's own
function. The residue is the spectrum divided by that rendered curve and
rounded, which makes the floor the quantisation step. Should a residue
exceed 4095, the floor is lifted until none does.

Stereo is coupled with square polar coupling, the exact inverse of the
decoder's step, and coded with residue type 2 over the interleaved
channels. Other channel counts use residue type 1, and their channels
are written in the Vorbis order, the inverse of the order the decoder
returns. A residue partition of 32 values falls into one of six classes
by its largest magnitude: silent, up to 1, 4, 15, 496 or 8190. The vector
books of the first four classes cover the integer grids of those ranges,
and the two largest classes code a value in cascade passes as multiples
of 16, or of 256 and 16, plus a rest. The classbook codes the classes of
two partitions per codeword. Every book receives a Huffman code built
from the counts of the first pass, with lengths up to 32 bits, and a
book with fewer than two used entries receives a second one, because
decoders read single entry books in different ways.

With `floor=0` the encoder writes floor 0 of order 12 and 24, which no
current libvorbis encoder produces. The target curve in decibels, raised
by the amplitude offset of 140 dB and limited to 60 dB below its
maximum, is sampled as the maximum of each Bark bin of the decoder's
map, with empty Bark bins interpolated. An LPC filter is fitted to its
square, and the roots of the sum and difference polynomials of the
filter, found by sign changes on 4096 angles and bisection, give the
line spectral pairs. Their cumulative angles are quantised in steps of
pi/256 and coded as increments with a scalar book. The amplitude retains
the decoder's curve at or below the target at all but 2% of the bins.

On a mono chime the encoder reaches 16, 33 and 54 dB signal-to-noise
ratio at quality -0.1, 0.4 and 1.0, and 51 dB with floor 0. On a stereo
signal with clicks, which produce short blocks, it reaches 27 dB at 0.4,
55 dB at 1.0 and 43 dB with floor 0, and on six channels 29 and 56 dB.
libvorbis and ffmpeg decode every file, and libvorbis returns the same
samples as the decoder of minios to within one step at 16 bits. Floor 0
decoding in minios is verified through these files.

## FLAC in Ogg

C7 added the codec `oggflac` to `flac.so`.

| Module | Codec | Capabilities | Probe |
|---|---|---|---|
| `flac.so` | `oggflac`, `audio/x-oggflac`, `.oga` | decode, encode | an Ogg stream whose first packet starts with `\x7fFLAC`, version 1, and contains `fLaC`, 100 or 90 |

The Ogg mapping of FLAC 1.0 puts a first packet at the start of a logical
stream with `\x7f` and `FLAC`, the mapping version 1.0, the number of
header packets that follow it (0 when unknown), the `fLaC` marker and the
STREAMINFO block with its header. One metadata block follows in each
further header packet and one frame in each audio packet. The granule
position of a page is the number of samples up to the end of the last
frame completed on it.

`oggflac.c` decodes by reassembly. `oggflac_open` reads the whole file
through the Ogg reader of libcodec and builds, for every accepted
logical stream, a native stream of the marker, STREAMINFO marked as the
last metadata block, and the frames. Header packets are recognised by
their count or, when the count is 0, by the absence of a frame sync code.
The native decoder then decodes the streams one after another and checks
the CRCs and the MD5 sum of each. Chained streams must have the format of
the first one, and a chained stream of another format ends the file. The
length is the sum of the totals in STREAMINFO. ffmpeg leaves the total at
0, and the length then comes from the granule positions. An error of the
Ogg reader is returned after the frames decoded before it.

The encoder takes the frames of the native encoder through
`flac_encode_stream`, which returns STREAMINFO, the frame bytes and the
end offset and sample count of every frame. It writes the first packet
alone on the first page and, on the second page, a VORBIS_COMMENT block
with the vendor string, which the mapping requires as the second header
packet. Each frame then becomes one packet with the sample count up to
its end as granule position.

The codec claims the extension `.oga`, the Xiph name for Ogg audio other
than Vorbis, and the MIME type `audio/x-oggflac`. `.ogg` and `audio/ogg`
remain with Vorbis, the format of most such files. Content identification
does not depend on these names, because the probes of all Ogg codecs use
`codec_ogg_probe`. It scores 100 when the first logical stream of the file
is accepted and 90 when a later stream of a multiplexed file is. The
first stream therefore decides the codec of a multiplexed file, and a
program opens another stream by naming its codec, as the Vorbis test does
for the multiplexed fixture.

## A new format: BMP

After the viewer and paint stopped naming formats (see below), adding
`bmp.so` required no change to any program. The viewer, paint, the
desktop wallpaper and the `codecs` command read BMP files as soon as the
module is installed in `/usr/lib/codecs`.

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

## JPEG

`jpeg.so` is the first module built on imported code. It compiles
`stb_image.h` and `stb_image_write.h` of the stb project from
`third_party/stb`, which are available under the public domain or the
MIT licence, without changes. The module restricts stb_image to JPEG and
turns off its standard I/O, SIMD, floating point and thread local
storage. Like every module, it exports only `codec_module`.

| Module | Codec | Capabilities | Probe |
|---|---|---|---|
| `jpeg.so` | `jpeg`, `image/jpeg image/pjpeg`, `.jpg .jpeg .jpe .jfif` | decode, encode | `FF D8 FF`, 90 |

The decoder reads baseline and progressive JPEG with Huffman coding, in
grey or in colour, up to 16384 pixels per side, and returns opaque
pixels. It does not support arithmetic coding, and it does not apply the
EXIF orientation, which means that a photo taken in portrait orientation
can appear sideways. The encoder writes baseline JPEG at quality 90 and
drops alpha, which JPEG cannot store. A picture that is encoded and
decoded again is therefore close to the original, not equal to it. The
case `codec_image` checks a round trip within a tolerance of four steps
per channel, and `codec_tool` converts PNG to JPEG and back with
`codecs`.

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
- `codecs convert [-f NAME] [-b BITS] [-s SIZE] [-o OPTIONS] IN OUT`
  decodes `IN` and encodes it with the codec called `NAME`, or with the
  encoder of the same kind that matches the extension of `OUT`. `-b` sets
  the sample size of converted audio, `-s` the size at which a vector
  image is rendered, and `-o` the options of an audio encoder. On success
  it prints `IN (codec) -> OUT (codec)`.

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
program, the registry remains empty, and the image functions return
`-ENOTSUP`. A static link against `libgui.a` must add `-lcodec`
(`tcc.md`).

The host unit tests compile libcodec and every module into the test
program with `-DCODEC_BUILTIN`. In this mode `CODEC_MODULE(name)` defines
a symbol called `codec_module_NAME`, and the registry registers the list
of built-in modules in `registry.c` instead of reading a directory. A new
module must be added to that list.

## The loader

Loading the modules one after another through a single path buffer
revealed a bug in `/lib/ld.so`. An object opened with `dlopen` retained a
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
that the listing shows the modules with their capabilities. It
converts an icon from PNG to BMP and back and verifies that the two BMP
files are identical byte for byte. It runs `info` on both files, on a BMP
file named `.png`, which must be identified by its content, and on a
generated SVG file. It converts with `-f` and renders an SVG file with
`-s`. It identifies the chime, converts it to 24 bits and back to 16, and
compares the result with a direct conversion. Finally it checks the
errors for unknown data, an extension without an encoder, formats of
different kinds, incorrect usage and a missing file. It also converts the
chime from WAV to FLAC and to Ogg FLAC and back and compares the results
with the original file.

With the argument `flac`, used by the boot test `codec_flac`, the
program decodes sixteen fixtures produced by `tools/gen_codec_fixtures.py`
with the reference `flac` encoder and with ffmpeg. They cover LPC of
order 32 with odd block sizes, 8, 12, 16, 20, 24 and 32 bit samples, three
and eight channels, wasted bits, the sample rate codes for kHz, Hz, tens
of Hz and STREAMINFO, a stream with variable block sizes assembled from
the frames of two encodes, and a stream behind an ID3v2 tag. Every
fixture must decode to the format and length recorded in STREAMINFO with
a matching MD5 sum. The test compares the shipped `chime.flac` with
`chime.wav`, encodes and decodes a test signal at every sample size from
4 to 32 bits with one, two, five and eight channels, and checks that a
damaged frame, a wrong MD5 sum, a cut file and a cut header are reported.
Its post script runs `flac -t` on five files written by the encoder in
minios.

With the argument `vorbis`, used by the boot test `codec_vorbis`, the
program decodes the eight Vorbis fixtures and their reference decodings
and requires the same sample rate, channel count and length, a length
equal to the one reported at open time, and a difference of at most one
step and an RMS difference below half a step at 16 bits. It also checks
that a changed byte in a page and a cut file are reported as damage.
`make check` runs `lib/libcodec/tests/test_codec.c` on the host, which checks
MD5 against the vectors of RFC 1321, the inverse MDCT against its
definition for all block sizes from 64 to 2048, and the FLAC and Vorbis
fixtures as above.

With the argument `vorbisenc`, used by the boot test `codec_vorbis_enc`,
the program encodes the chime at quality -0.1, 0.4 and 1.0 and with floor
0, a stereo signal with clicks and a 5.1 signal at 0.4 and 1.0, and the
stereo signal with floor 0. Every file must decode in minios to the
length of the input with a signal-to-noise ratio above a limit a few dB
below the values measured on the host, and a higher quality must give a
larger file. The program also checks the rejection of a quality of 2, of
an unknown option, of options for a codec without options, and of floor
0 above 65535 Hz. Five of the files are retained with their decoding by
minios, and the post script decodes them with libvorbis on the host,
requires the same samples to within one step, and decodes them with
ffmpeg as well.

With the argument `oggflac`, used by the boot test `codec_oggflac`, the
program decodes an Ogg FLAC file from `flac --ogg`, one from the Ogg muxer
of ffmpeg, two chained streams, and the FLAC stream of the multiplexed
Vorbis fixture, which identification by content must select. It requires
the lengths, the formats and the lengths reported at open time. It
encodes 16 bit mono, 24 bit stereo and 8 bit eight-channel signals,
requires identical samples after decoding, retains the files for the post
script, and checks that a changed byte in a page is reported. The post
script tests the files with `flac -t` and ffmpeg on the host.

`gui_images` converts the saved drawing to BMP with `codecs` and shows
the BMP file in the viewer. Its post script decodes the BMP file with its
own reader and compares the pixels with the PNG file. `audio_player` covers the player.
