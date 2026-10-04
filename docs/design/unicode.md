# Unicode character data

The character classes, the simple case mappings and the display widths of
the C library come from the Unicode Character Database, version 16.0.
`tools/fetch_unicode.sh` downloads `UnicodeData.txt`,
`EastAsianWidth.txt`, `DerivedCoreProperties.txt` and `PropList.txt` with
the Unicode licence into `third_party/unicode`. `tools/genunicode.py` reads
them and writes `lib/libc/src/wchar/unidata.h`, which is checked in. The header
must be generated again after a change of the data or the generator.

## Tables

The header contains seven sorted tables of inclusive code point ranges and
two tables of case runs. `wctype.c` and `wcwidth.c` search each table with a
binary search. The tables contain about 3,800 ranges and take about 37 KB of
`libc.so`.

| Table | Content |
|---|---|
| `uni_alpha` | The property Alphabetic. In ASCII it contains the letters only. |
| `uni_upper`, `uni_lower` | The properties Uppercase and Lowercase. |
| `uni_punct` | The general categories P (punctuation) and S (symbols). |
| `uni_print` | Every assigned code point except the categories Cc and Cs. |
| `uni_zero` | Width 0: the categories Mn, Me and Cf except U+00AD, the Hangul medial vowels and final consonants U+1160 to U+11FF and U+D7B0 to U+D7FF, and the default ignorable code points. |
| `uni_wide` | Width 2: East Asian Width W and F, without the code points of `uni_zero`. |
| `uni_toupper`, `uni_tolower` | The simple case mappings of fields 12 and 13 of `UnicodeData.txt`. |

A case run `{first, last, delta, stride}` maps every code point
`first + k * stride` up to `last` to the code point plus `delta`. A stride
of 2 covers the alternating upper and lower case pairs of Latin Extended-A
and Cyrillic in one run.

## Classes

`iswalpha`, `iswupper`, `iswlower`, `iswpunct` and `iswprint` use the tables.
`iswdigit` and `iswxdigit` contain the ASCII digits only, as POSIX requires
for the C locale. `iswspace` contains the ASCII white space, U+0085, U+1680,
U+2000 to U+2006, U+2008 to U+200A, U+2028, U+2029, U+205F and U+3000. The
no-break spaces U+00A0, U+2007 and U+202F are printable and are neither
space nor graph, as in glibc and musl. Before this change `iswspace(0xa0)`
returned 1. `iswcntrl` contains the C0 and C1 controls and the line and
paragraph separators.

`towupper` and `towlower` apply the simple mappings only. A mapping that
changes the length of the text, such as U+00DF to "SS", is not applied, so
`towupper(0xdf)` returns U+00DF.

## Widths

`wcwidth` returns 0 for the null character and the code points of
`uni_zero`, -1 for controls, surrogates, unassigned code points and values
above U+10FFFF, 2 for the code points of `uni_wide` and 1 for every other
character. `wcswidth` adds the widths and returns -1 when one of them is -1.

## Terminal cells

The terminal emulator (`user/term/vt.c`) takes the width of every printed
character from `wcwidth`. A character of width 2 occupies its cell and the
next cell, whose code point is `VC_WIDE_TAIL` (0x110000). When the cursor is
in the last column, the emulator clears that cell and writes the character
at the start of the next line. Overwriting one half of a wide character
clears the other half. A character of width 0 becomes the `mark` of the
previous cell, and a cell stores one mark. The window draws the character
and its mark as one string, and the font centres the mark over the base
character. The window does not draw the right half of a wide character. Copying a
selection writes the character and its mark and omits the right half.

## Tests

The boot test `unicode_data` runs `/bin/unicodetest`. It checks the widths of
18 code points, the classes of letters of eight scripts, punctuation,
symbols, spaces and unassigned code points, 14 case mappings including the
final sigma, the long s, the dotted capital I and a Deseret letter, and the
cells of the terminal emulator for wide characters, marks, overwritten
halves and wrapping at the right margin. The source of the emulator is
compiled into the test program, because the emulator has no drawing code.
