#!/usr/bin/env python3
"""Write the candidate tables of the input methods of the compositor (L6,
docs/design/ime.md) from the Unihan database in third_party/unihan, which
tools/fetch_unihan.sh downloads.

user/share/ime/kana.tab lists, for each reading in hiragana, the kanji with
that reading: the Jōyō kanji (kJoyoKanji) first, then the others.  Within
each group a kanji whose list of readings in kJapanese names the reading
earlier comes first, then a kanji with fewer readings, then the lower code
point.  Katakana readings of kJapanese (the on readings) are converted to
hiragana.

user/share/ime/pinyin.tab lists, for each syllable of pinyin without tone
marks (ü written as v), the characters of kHanyuPinlu with that reading,
ordered by the frequency that kHanyuPinlu gives.  Traditional characters,
which have a kSimplifiedVariant other than themselves, are left out.

Each line is the reading, a tab and the characters without separators."""
import os
import re
import unicodedata

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UNIHAN = os.path.join(TOP, 'third_party', 'unihan')
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


def toneless(syllable):
    s = unicodedata.normalize('NFD', syllable)
    s = s.replace('ü', 'v').replace('Ü', 'v')
    return ''.join(c for c in s if not unicodedata.combining(c)).lower()


def main():
    os.makedirs(OUT, exist_ok=True)
    readings = fields('Unihan_Readings.txt', {'kJapanese', 'kHanyuPinlu'})
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

    variants = fields('Unihan_Variants.txt', {'kSimplifiedVariant'}).get('kSimplifiedVariant', {})
    traditional = {cp for cp, v in variants.items() if any(int(x[2:], 16) != cp for x in v.split())}
    pinyin = {}
    for cp, value in readings.get('kHanyuPinlu', {}).items():
        if cp in traditional:
            continue
        for m in re.finditer(r'([^\s(]+)\((\d+)\)', value):
            pinyin.setdefault(toneless(m.group(1)), {})
            freq = pinyin[toneless(m.group(1))]
            freq[cp] = max(freq.get(cp, 0), int(m.group(2)))
    with open(os.path.join(OUT, 'pinyin.tab'), 'w', encoding='utf-8') as f:
        f.write('# Hanzi by syllable, written by tools/genime.py from kHanyuPinlu of Unihan 16.0.\n')
        for key in sorted(pinyin):
            chars = sorted(pinyin[key], key=lambda c: (-pinyin[key][c], c))
            f.write(key + '\t' + ''.join(chr(c) for c in chars) + '\n')
    print(f'genime: {len(kana)} readings in kana.tab, {len(pinyin)} syllables in pinyin.tab')


if __name__ == '__main__':
    main()
