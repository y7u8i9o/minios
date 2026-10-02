#!/usr/bin/env python3
"""Generate libc/src/wchar/unidata.h from the Unicode Character Database
in third_party/unicode (tools/fetch_unicode.sh downloads it).

The header contains sorted tables of code point ranges for the character
classes and the widths, and a table of simple case mappings.  The C library
searches each table with a binary search (libc/src/wchar/wctype.c and
wcwidth.c)."""
import os

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UCD = os.path.join(TOP, 'third_party', 'unicode')
OUT = os.path.join(TOP, 'libc', 'src', 'wchar', 'unidata.h')


def read_unicode_data():
    """Return the general category, the simple case mappings and the
    canonical decompositions of every assigned code point.  Ranges given as
    First and Last lines are expanded."""
    category, upper, lower, decomp = {}, {}, {}, {}
    first = None
    with open(os.path.join(UCD, 'UnicodeData.txt')) as f:
        for line in f:
            fields = line.rstrip('\n').split(';')
            cp, name, cat = int(fields[0], 16), fields[1], fields[2]
            if name.endswith(', First>'):
                first = cp
                continue
            if name.endswith(', Last>'):
                for c in range(first, cp + 1):
                    category[c] = cat
                first = None
                continue
            category[cp] = cat
            if fields[5] and not fields[5].startswith('<'):
                decomp[cp] = [int(x, 16) for x in fields[5].split()]
            if fields[12]:
                upper[cp] = int(fields[12], 16)
            if fields[13]:
                lower[cp] = int(fields[13], 16)
    return category, upper, lower, decomp


def base_letters(category, decomp):
    """Return (code point, base letter, first mark) for the letters of the
    Latin, Greek and Cyrillic blocks whose full canonical decomposition is
    a letter followed by combining marks."""
    def full(c):
        if c not in decomp:
            return [c]
        out = []
        for d in decomp[c]:
            out += full(d)
        return out
    blocks = [(0xc0, 0x24f), (0x370, 0x3ff), (0x400, 0x52f), (0x1e00, 0x1fff)]
    out = []
    for c in sorted(decomp):
        if not any(a <= c <= b for a, b in blocks) or not category.get(c, '').startswith('L'):
            continue
        seq = full(c)
        if len(seq) >= 2 and category.get(seq[0], '').startswith('L') and all(category.get(m) == 'Mn' for m in seq[1:]):
            out.append((c, seq[0], seq[1]))
    return out


def read_property(filename, wanted):
    """Return the set of code points that have the property wanted in a
    file of the form "XXXX..YYYY ; Property # comment"."""
    out = set()
    with open(os.path.join(UCD, filename)) as f:
        for line in f:
            line = line.split('#')[0].strip()
            if not line:
                continue
            rng, prop = [x.strip() for x in line.split(';')[:2]]
            if prop != wanted:
                continue
            if '..' in rng:
                a, b = rng.split('..')
                out.update(range(int(a, 16), int(b, 16) + 1))
            else:
                out.add(int(rng, 16))
    return out


def ranges(points):
    """Merge a set of code points into sorted inclusive ranges."""
    out = []
    for c in sorted(points):
        if out and out[-1][1] + 1 == c:
            out[-1][1] = c
        else:
            out.append([c, c])
    return out


def case_runs(mapping):
    """Compress a case mapping into runs (first, last, delta, stride): every
    code point first + k * stride up to last maps to itself plus delta."""
    runs = []
    for c in sorted(mapping):
        d = mapping[c] - c
        if runs:
            first, last, delta, stride = runs[-1]
            if delta == d and stride == 0 and c - last in (1, 2):
                runs[-1] = [first, c, delta, c - last]
                continue
            if delta == d and stride and c - last == stride:
                runs[-1][1] = c
                continue
        runs.append([c, c, d, 0])
    for r in runs:
        if r[3] == 0:
            r[3] = 1
    return runs


def emit_ranges(out, name, comment, rs):
    out.append(f'/* {comment} */')
    out.append(f'static const struct uni_range {name}[] = {{')
    for i in range(0, len(rs), 4):
        out.append('    ' + ' '.join(f'{{0x{a:x}, 0x{b:x}}},' for a, b in rs[i:i + 4]))
    out.append('};')
    out.append('')


def emit_case(out, name, comment, runs):
    out.append(f'/* {comment} */')
    out.append(f'static const struct uni_case {name}[] = {{')
    for i in range(0, len(runs), 3):
        out.append('    ' + ' '.join(f'{{0x{a:x}, 0x{b:x}, {d}, {s}}},' for a, b, d, s in runs[i:i + 3]))
    out.append('};')
    out.append('')


def main():
    version = open(os.path.join(UCD, 'VERSION')).read().strip()
    category, upper, lower, decomp = read_unicode_data()
    alpha = read_property('DerivedCoreProperties.txt', 'Alphabetic')
    uppercase = read_property('DerivedCoreProperties.txt', 'Uppercase')
    lowercase = read_property('DerivedCoreProperties.txt', 'Lowercase')
    default_ignorable = read_property('DerivedCoreProperties.txt', 'Default_Ignorable_Code_Point')

    wide = set()
    with open(os.path.join(UCD, 'EastAsianWidth.txt')) as f:
        for line in f:
            line = line.split('#')[0].strip()
            if not line:
                continue
            rng, prop = [x.strip() for x in line.split(';')[:2]]
            if prop not in ('W', 'F'):
                continue
            a, _, b = rng.partition('..')
            wide.update(range(int(a, 16), int(b or a, 16) + 1))

    # Zero width: nonspacing and enclosing marks, format controls other
    # than the soft hyphen, the Hangul medial vowels and final consonants,
    # and the default ignorable code points.
    zero = {c for c, cat in category.items() if cat in ('Mn', 'Me') or (cat == 'Cf' and c != 0xad)}
    zero |= set(range(0x1160, 0x1200)) | set(range(0xd7b0, 0xd800))
    zero |= {c for c in default_ignorable if category.get(c, 'Cn') != 'Cn' or c >= 0xe0000}
    zero.discard(0xad)
    wide -= zero

    punct = {c for c, cat in category.items() if cat[0] in 'PS'}
    # Printable: every assigned code point except controls and surrogates.
    printable = {c for c, cat in category.items() if cat not in ('Cc', 'Cs')}
    # The C locale uses the POSIX ASCII classes: digits are 0 to 9 only,
    # and the ASCII letters are the only alphabetic ASCII characters.
    alpha = {c for c in alpha if c >= 0x80 or chr(c).isalpha()}

    to_upper = {c: u for c, u in upper.items()}
    to_lower = {c: l for c, l in lower.items()}

    out = [
        f'/* Generated by tools/genunicode.py from the Unicode Character Database',
        f' * {version} in third_party/unicode. Do not edit. */',
        '#pragma once',
        '#include <stdint.h>',
        '',
        f'#define UNI_VERSION "{version}"',
        '',
        'struct uni_range {',
        '    uint32_t first, last;',
        '};',
        '',
        '/* A base letter entry gives the letter and the first combining mark of',
        ' * the canonical decomposition of cp, for collation. */',
        'struct uni_base {',
        '    uint32_t cp, base, mark;',
        '};',
        '',
        '/* A case run maps first, first + stride, ... up to last to the code',
        ' * point plus delta. */',
        'struct uni_case {',
        '    uint32_t first, last;',
        '    int32_t delta;',
        '    uint32_t stride;',
        '};',
        '',
    ]
    emit_ranges(out, 'uni_alpha', 'Alphabetic (DerivedCoreProperties.txt).', ranges(alpha))
    emit_ranges(out, 'uni_upper', 'Uppercase (DerivedCoreProperties.txt).', ranges(uppercase))
    emit_ranges(out, 'uni_lower', 'Lowercase (DerivedCoreProperties.txt).', ranges(lowercase))
    emit_ranges(out, 'uni_punct', 'Punctuation and symbols: general categories P and S.', ranges(punct))
    emit_ranges(out, 'uni_print', 'Assigned code points other than controls and surrogates.', ranges(printable))
    emit_ranges(out, 'uni_zero', 'Code points of width 0: marks, format controls, Hangul medial and final jamo.',
                ranges(zero))
    emit_ranges(out, 'uni_wide', 'Code points of width 2: East Asian Width W and F.', ranges(wide))
    emit_case(out, 'uni_toupper', 'Simple uppercase mappings (UnicodeData.txt field 12).', case_runs(to_upper))
    emit_case(out, 'uni_tolower', 'Simple lowercase mappings (UnicodeData.txt field 13).', case_runs(to_lower))
    bases = base_letters(category, decomp)
    out.append('/* Latin, Greek and Cyrillic letters with their base letter and first mark. */')
    out.append('static const struct uni_base uni_bases[] = {')
    for i in range(0, len(bases), 4):
        out.append('    ' + ' '.join(f'{{0x{a:x}, 0x{b:x}, 0x{m:x}}},' for a, b, m in bases[i:i + 4]))
    out.append('};')
    out.append('')
    with open(OUT, 'w') as f:
        f.write('\n'.join(out))
    print(f'genunicode: wrote {OUT}')


if __name__ == '__main__':
    main()
