# Font rendering

M20 adds `libfont/` (`libfont.a`), a library independent from the window
system that parses TrueType and CFF flavoured OpenType files, rasterizes
outlines to 8 bit coverage bitmaps with fixed point arithmetic, and
reads kerning from the `kern` and `GPOS` tables. libgui wraps it behind
its `struct font`, so the toolkit and the applications use outline fonts
through the same calls as the bitmap fonts of M19. User programs are
compiled without SSE and x87, so no floating point appears anywhere.

## Parser (`libfont/src/ttf.c`, `cff.c`)

- `font_open` reads the whole file, accepts the sfnt versions
  `0x00010000`, `true` and `OTTO`, and locates `head` (units per em, loca
  format), `hhea` (ascent, descent, line gap, number of horizontal
  metrics), `maxp` (glyph count), `hmtx`, `cmap`, `loca` and `glyf` or
  `CFF `, `kern` and `GPOS`. Every table access is bounds checked
  against the file size.
- The character map prefers a format 12 subtable (Unicode full), then
  format 4, then formats 0 and 6. `font_glyph_index` returns glyph 0 for
  unmapped code points.
- `font_outline` returns contours of points in font units. `glyf`
  outlines retain TrueType's on-curve and quadratic control points;
  composite glyphs are flattened by applying each component's offset and
  2.14 transform (point matching is not supported and treated as no
  offset), to a depth of eight. CFF outlines come from a Type 2
  charstring interpreter (`cff.c`): INDEX and DICT structures, the
  Top and Private DICTs, global and local subroutines with the standard
  bias, the width prefix on the first stack clearing operator, all
  moveto, lineto and curveto families, flex operators, and hint operators
  (`hstem`, `vstem`, `hintmask`, `cntrmask`) whose data is skipped. Cubic
  control points are marked as such so the rasterizer flattens them
  directly. CID keyed fonts select the Private DICT through `FDSelect`
  formats 0 and 3. `seac` accents and arithmetic operators are not
  supported.

## Rasterizer (`libfont/src/raster.c`)

- Points are scaled to 26.6 pixels with `font_scale` (rounded, 64 bit
  intermediate) and flipped so y grows downwards. Quadratic and cubic
  curves are flattened into two to twenty four segments depending on
  their extent (one segment per three pixels), using 16.16 and 12 bit
  parameters so every product fits in 64 bits.
- Edges are scan converted per pixel row with four sub scanlines. On
  each sub scanline the crossings of all edges are sorted by x, the non
  zero winding rule selects the covered spans, and each span adds its
  exact horizontal overlap (in 1/64 pixel) to a row accumulator, which
  is then scaled to 0..255. The result is `struct font_glyph`: the
  bitmap, its bearing (`left`, `top` relative to the pen with y down)
  and the advance in 26.6 pixels.
- `font_render` retains up to `FONT_CACHE_SIZE` (512) bitmaps per font,
  keyed by glyph and pixel size, replacing the least recently used one.

## Kerning and shaping (`libfont/src/kern.c`)

- `font_kern` consults `GPOS` first: the `kern` feature's lookups of
  type 2 (pair adjustment, also reached through type 9 extension
  lookups), format 1 (pair sets) and format 2 (class based, class
  definition formats 1 and 2), coverage formats 1 and 2, reading the
  X advance of the first value record. Without a `GPOS` pair the `kern`
  table (version 0, format 0 subtables, binary search) is used.
- `font_shape` decodes UTF-8 code points, maps them through the selected
  Unicode cmap, and returns glyph ids and pen positions in 26.6 pixels,
  adding the kerning of each pair. Combining marks are positioned at the
  current cluster. libgui uses the shaped result for drawing, measuring
  and cursor placement and takes an unmapped code point from the chain of
  fallback fonts and, for CJK characters, from the CJK font that
  `i18n.md` describes.

## Integration (`libgui/src/font.c`)

- `gfx_font_open_ttf(path, px)` fills a `struct font` whose `outline`
  member points at the `struct ofont`; `height` and `ascent` come from
  `hhea`, and the per character `advance` table from the scaled
  advances (used only by callers that read it directly).
- `gfx_text_font` shapes the UTF-8 string and blends each cached bitmap with
  `gfx_blend_mask` (alpha over the surface contents, or over the
  background colour when one is given). `gfx_text_width_font` and
  `gfx_text_index_font` use shaped positions. Bitmap fonts follow the
  previous code path, so `term`, the toolkit and the applications work
  with either kind.
- `/usr/share/fonts/` contains `DejaVuSans.ttf`, `DejaVuSansMono.ttf` (Bitstream
  Vera license, `third_party/dejavu/LICENSE`),
  `lmroman10-regular.otf` (GUST Font License,
  `third_party/lmodern/NOTICE`), `NotoSans-Regular.ttf` (SIL Open
  Font License 1.1, `third_party/notosans/NOTICE`) and `unifont.otf`
  (GNU Unifont 17.0.05, dual SIL Open Font License 1.1 and GPL 2 or
  later with the font embedding exception,
  `third_party/unifont/NOTICE`). `view` offers an "Outline font" menu
  entry.
- The four text fonts cover the scripts their families were drawn for.
  Noto Sans carries 3094 code points (Latin, Greek, Cyrillic, phonetics
  and punctuation); the Noto project retains CJK, Hangul, kana, Hebrew,
  Arabic and emoji in separate families that are not installed here.
  DejaVu Sans carries 5906. Unifont carries 58910, covering the Basic
  Multilingual Plane and 1840 code points between U+1F12F and U+3237F,
  which is why the Unicode viewer uses it as the default glyph source.
- Unifont has 64 units per em because its outlines are traced from a 16
  pixel bitmap grid. `font_scale` multiplies in `int64_t` before
  dividing by the em, so this em size costs no precision, and the
  Unicode viewer renders at 32 and 64 pixels so that the bitmap grid
  falls on whole pixels.

## Tests

- `ttf`: `/bin/fonttest` opens both fonts and checks the CFF flag, units
  per em, glyph counts, the glyph id and advance of `A`, the kerning of
  `AV`, `To` and `oo` against values computed with fontTools, rejection
  of unmapped code points, the contour count and bounds of `A`, the
  rasterized `A` at 32 pixels (bitmap size derived from the scaled
  bounds, bearing, advance, counts of fully and partially covered
  pixels, coverage sum), the cache, shaping of `AV`, and rejection of a
  bitmap font file and a missing file. It then checks Unifont (CFF flag,
  64 units per em, 58911 glyphs, coverage of CJK, hiragana, Hebrew,
  Arabic, Hangul, Devanagari, Georgian and U+2000B above the BMP, the
  absence of emoji, and the 22x32 rasterization of U+4E2D at 32 pixels)
  and the scope of Noto Sans (Latin and Cyrillic present, CJK, kana and
  Hangul absent).
- `gui_ttf`: `guitest ttf` draws a string at 32 pixels in a white
  window; the kernel counts black, grey and white pixels in the text
  rows, requiring more than 200 grey (antialiased) pixels, and logs a
  CRC.

The text-input protocol carries committed UTF-8, preedit and delete
events. In the built-in X12 input method, Ctrl+Shift+U starts a visible
hexadecimal preedit; Enter or Space commits the code point and Escape
cancels it.
- The host harness used during development (a small program compiled
  with the system compiler against `libfont/src/*.c`) is not part of the
  tree; the library has no dependencies beyond `stdio`, `stdlib` and
  `string`, so it compiles unchanged on the host.

## Host test

`make check` runs `libfont/tests/test_raster.c` against the bundled
DejaVu Sans: narrow glyphs (`.`, `!`, `i`, `l`, `:`) and `o` must get
coverage at 12 to 40 pixels. It guards the scan converter's crossing
sort, which once compared a shifted x with an unshifted one and left
glyphs whose left edge lies past the first pixel column empty (`.` at
28 px, `!` at 14 px).
