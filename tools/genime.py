#!/usr/bin/env python3
"""Write the dictionaries of the input methods (docs/design/ime.md).

user/share/ime/pinyin.dict, for the pinyin engine of imed (I3), comes from
pinyin_simp.dict.yaml of rime-pinyin-simp in third_party/imedata, which
tools/fetch_imedata.sh downloads.  It is a little endian binary file:

    "MPY1", then the uint32 values nsyl, nentries, syllable table offset,
    entry table offset, pool offset and pool size;
    the syllables in alphabetical order, 8 bytes each, padded with zeros;
    the entries, 16 bytes each: four uint16 syllable numbers (0xffff after
    the last), a uint32 weight and the uint32 offset of the word in the
    pool, sorted by the syllable numbers and then by falling weight;
    the pool of the words in UTF-8, each ended by a zero byte.

Because the syllables are sorted, the syllables that begin with some
letters have consecutive numbers, which the engine uses for incomplete
syllables and abbreviations.

user/share/ime/kana.tab, for the Japanese engine of the compositor (L6),
comes from the Unihan database in third_party/unihan, which
tools/fetch_unihan.sh downloads.

user/share/ime/kana.tab lists, for each reading in hiragana, the kanji with
that reading: the Jōyō kanji (kJoyoKanji) first, then the others.  Within
each group a kanji whose list of readings in kJapanese names the reading
earlier comes first, then a kanji with fewer readings, then the lower code
point.  Katakana readings of kJapanese (the on readings) are converted to
hiragana.

Each line of kana.tab is the reading, a tab and the characters without
separators."""
import os
import re
import shutil
import struct

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UNIHAN = os.path.join(TOP, 'third_party', 'unihan')
IMEDATA = os.path.join(TOP, 'third_party', 'imedata')
OUT = os.path.join(TOP, 'user', 'share', 'ime')


def fields(filename, wanted):
    out = {}
    with open(os.path.join(UNIHAN, filename), encoding='utf-8') as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            cp, field, value = line.rstrip('\n').split('\t', 2)
            if field in wanted:
                out.setdefault(field, {})[int(cp[2:], 16)] = value
    return out


def to_hiragana(s):
    return ''.join(chr(ord(c) - 0x60) if 0x30a1 <= ord(c) <= 0x30f6 else c for c in s)


def write_pinyin():
    """pinyin.dict from pinyin_simp.dict.yaml."""
    words = {}
    started = False
    with open(os.path.join(IMEDATA, 'pinyin_simp.dict.yaml'), encoding='utf-8') as f:
        for line in f:
            if line.startswith('...'):
                started = True
                continue
            if not started or line.startswith('#') or not line.strip():
                continue
            fields = line.rstrip('\n').split('\t')
            if len(fields) < 2:
                continue
            syllables = tuple(fields[1].split())
            weight = int(fields[2]) if len(fields) > 2 and fields[2].strip() else 0
            if not 1 <= len(syllables) <= 4:
                continue
            key = (fields[0], syllables)
            words[key] = max(words.get(key, 0), weight)
    names = sorted({s for _, syl in words for s in syl})
    number = {name: i for i, name in enumerate(names)}
    entries = sorted(((tuple(number[s] for s in syl), -w, word) for (word, syl), w in words.items()))
    pool = bytearray()
    records = bytearray()
    offsets = {}
    for ids, negw, word in entries:
        if word not in offsets:
            offsets[word] = len(pool)
            pool += word.encode('utf-8') + b'\0'
        padded = list(ids) + [0xffff] * (4 - len(ids))
        records += struct.pack('<4HII', *padded, -negw, offsets[word])
    syl_table = b''.join(name.encode('ascii').ljust(8, b'\0') for name in names)
    header = 4 + 6 * 4
    syl_off = header
    entry_off = syl_off + len(syl_table)
    pool_off = entry_off + len(records)
    data = b'MPY1' + struct.pack('<6I', len(names), len(entries), syl_off, entry_off, pool_off, len(pool))
    data += syl_table + records + pool
    with open(os.path.join(OUT, 'pinyin.dict'), 'wb') as f:
        f.write(data)
    shutil.copy(os.path.join(IMEDATA, 'pinyin_simp.LICENSE'), os.path.join(OUT, 'pinyin_simp.LICENSE'))
    print(f'genime: {len(names)} syllables, {len(entries)} entries, {len(data)} bytes in pinyin.dict')


def main():
    os.makedirs(OUT, exist_ok=True)
    readings = fields('Unihan_Readings.txt', {'kJapanese'})
    joyo = set(fields('Unihan_OtherMappings.txt', {'kJoyoKanji'}).get('kJoyoKanji', {}))

    kana = {}
    for cp, value in readings.get('kJapanese', {}).items():
        words = value.split()
        for position, reading in enumerate(words):
            key = to_hiragana(reading)
            if re.fullmatch(r'[ぁ-ゖー]+', key) and cp not in kana.get(key, {}):
                kana.setdefault(key, {})[cp] = (cp not in joyo, position, len(words), cp)
    with open(os.path.join(OUT, 'kana.tab'), 'w', encoding='utf-8') as f:
        f.write('# Kanji by reading, written by tools/genime.py from Unihan 16.0.\n')
        for key in sorted(kana):
            chars = sorted(kana[key], key=lambda c: kana[key][c])
            f.write(key + '\t' + ''.join(chr(c) for c in chars) + '\n')

    print(f'genime: {len(kana)} readings in kana.tab')
    write_pinyin()


if __name__ == '__main__':
    main()
