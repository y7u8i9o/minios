#!/usr/bin/env python3
"""Write the dictionaries of the input methods (docs/design/ime.md).

user/share/ime/pinyin.dict, for the pinyin engine of imed (I3), comes from
pinyin_simp.dict.yaml of rime-pinyin-simp in third_party/imedata, which
tools/fetch_imedata.sh downloads.  It is a little endian binary file:

    "MPY1", then the uint32 values nsyl, nentries, syllable table offset,
    entry table offset, pool offset, pool size, and the sum of the
    weights plus one for each entry;
    the syllables in alphabetical order, 8 bytes each, padded with zeros;
    the entries, 16 bytes each: four uint16 syllable numbers (0xffff after
    the last), a uint32 weight and the uint32 offset of the word in the
    pool, sorted by the syllable numbers and then by falling weight;
    the pool of the words in UTF-8, each ended by a zero byte.

Because the syllables are sorted, the syllables that begin with some
letters have consecutive numbers, which the engine uses for incomplete
syllables and abbreviations.

user/share/ime/japanese.dict, for the Japanese engine of imed (I4), comes
from dictionary_oss of Mozc in third_party/imedata.  The 2672 part of
speech ids become about 1430 classes: a verb or an adjective without a
word of its own retains its group, conjugation type and conjugation form,
every other id remains a class (particles, auxiliary verbs, nouns, and the
verbs with a word such as いる or 来る).  The connection cost between two
classes is the mean of the costs between their ids, stored in steps of
60.  The entries with a
cost below 6000 and every particle and auxiliary verb are retained.  The file
is little endian:

    "MJP1", then the uint32 values nclass, nreadings, nentries, the class
    of the start and end of a sentence, the offsets of the class flags,
    the matrix, the readings, the entries and the pool, the pool size,
    and the class of common nouns, which unknown characters take;
    one byte of flags per class: 1 particle or auxiliary verb, 2 suffix or
    dependent word, 4 prefix;
    the matrix of uint8 connection costs in steps of 60, [class of the
    left word's right side][class of the right word's left side];
    the readings in byte order, 8 bytes each: the uint32 offset of the
    reading in the pool and the uint32 number of its first entry (the next
    reading's first entry ends its entries);
    the entries, 10 bytes each: the uint32 offset of the surface in the
    pool (0xffffffff: the reading itself, 0xfffffffe: the reading in
    katakana), the uint16 left and right classes, and the int16 cost;
    the pool of zero-terminated UTF-8 strings.
"""
import os
import shutil
import struct

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMEDATA = os.path.join(TOP, 'third_party', 'imedata')
OUT = os.path.join(TOP, 'user', 'share', 'ime')


def katakana(s):
    return ''.join(chr(ord(c) + 0x60) if 0x3041 <= ord(c) <= 0x3096 else c for c in s)


def pos_class(fields):
    if fields[0] in ('動詞', '形容詞') and not (fields[1] == '自立' and fields[6] != '*'):
        return (fields[0], fields[1], fields[4], fields[5])
    return tuple(fields)


def write_japanese():
    """japanese.dict from the Mozc dictionary."""
    ids = {}
    with open(os.path.join(IMEDATA, 'mozc.id.def'), encoding='utf-8') as f:
        for line in f:
            number, pos = line.split(' ', 1)
            ids[int(number)] = pos.strip().split(',')
    keys = sorted({pos_class(v) for v in ids.values()})
    class_of_key = {k: i for i, k in enumerate(keys)}
    cls = [class_of_key[pos_class(ids[i])] for i in range(len(ids))]
    flags = bytearray(len(keys))
    for k, i in class_of_key.items():
        if k[0] in ('助詞', '助動詞'):
            flags[i] |= 1
        if k[1] in ('接尾', '非自立'):
            flags[i] |= 2
        if k[0] == '接頭詞':
            flags[i] |= 4
    nclass = len(keys)
    sums = [[0] * nclass for _ in range(nclass)]
    counts = [[0] * nclass for _ in range(nclass)]
    with open(os.path.join(IMEDATA, 'mozc.connection_single_column.txt')) as f:
        size = int(f.readline())
        for left in range(size):
            row_sum, row_count, cl = sums[cls[left]], counts[cls[left]], cls
            for right in range(size):
                cost = int(f.readline())
                row_sum[cl[right]] += cost
                row_count[cl[right]] += 1
    matrix = bytearray()
    for a in range(nclass):
        for b in range(nclass):
            mean = sums[a][b] // counts[a][b] if counts[a][b] else 15300
            matrix.append(max(0, min(255, (mean + 30) // 60)))
    words = {}
    for i in range(10):
        with open(os.path.join(IMEDATA, f'mozc.dictionary0{i}.txt'), encoding='utf-8') as f:
            for line in f:
                fields = line.rstrip('\n').split('\t')
                if len(fields) < 5:
                    continue
                reading, lid, rid, cost, surface = fields[0], int(fields[1]), int(fields[2]), int(fields[3]), fields[4]
                function = ids[lid][0] in ('助詞', '助動詞')
                if cost >= 6000 and not function:
                    continue
                key = (reading, surface, cls[lid], cls[rid])
                if key not in words or cost < words[key]:
                    words[key] = cost
    by_reading = {}
    for (reading, surface, lc, rc), cost in words.items():
        by_reading.setdefault(reading, []).append((cost, surface, lc, rc))
    pool = bytearray()
    offsets = {}

    def intern(text):
        if text not in offsets:
            offsets[text] = len(pool)
            pool.extend(text.encode('utf-8') + b'\0')
        return offsets[text]

    readings = sorted(by_reading, key=lambda r: r.encode('utf-8'))
    reading_table = bytearray()
    entry_table = bytearray()
    nentries = 0
    for reading in readings:
        reading_table += struct.pack('<II', intern(reading), nentries)
        for cost, surface, lc, rc in sorted(by_reading[reading]):
            if surface == reading:
                where = 0xffffffff
            elif surface == katakana(reading):
                where = 0xfffffffe
            else:
                where = intern(surface)
            entry_table += struct.pack('<IHHh', where, lc, rc, cost)
            nentries += 1
    bos = cls[0]
    header = 4 + 11 * 4
    flag_off = header
    matrix_off = flag_off + len(flags)
    reading_off = matrix_off + len(matrix)
    entry_off = reading_off + len(reading_table)
    pool_off = entry_off + len(entry_table)
    noun = cls[1851]                                    # 名詞,一般,*,*,*,*,*
    data = b'MJP1' + struct.pack('<11I', nclass, len(readings), nentries, bos, flag_off, matrix_off, reading_off,
                                 entry_off, pool_off, len(pool), noun)
    data += bytes(flags) + bytes(matrix) + bytes(reading_table) + bytes(entry_table) + bytes(pool)
    with open(os.path.join(OUT, 'japanese.dict'), 'wb') as f:
        f.write(data)
    shutil.copy(os.path.join(IMEDATA, 'mozc.README.txt'), os.path.join(OUT, 'mozc.README.txt'))
    print(f'genime: {nclass} classes, {len(readings)} readings, {nentries} entries, {len(data)} bytes in japanese.dict')


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
    header = 4 + 7 * 4
    syl_off = header
    entry_off = syl_off + len(syl_table)
    pool_off = entry_off + len(records)
    total = min(0xffffffff, sum(1 - negw for _, negw, _ in entries))
    data = b'MPY1' + struct.pack('<7I', len(names), len(entries), syl_off, entry_off, pool_off, len(pool), total)
    data += syl_table + records + pool
    with open(os.path.join(OUT, 'pinyin.dict'), 'wb') as f:
        f.write(data)
    shutil.copy(os.path.join(IMEDATA, 'pinyin_simp.LICENSE'), os.path.join(OUT, 'pinyin_simp.LICENSE'))
    print(f'genime: {len(names)} syllables, {len(entries)} entries, {len(data)} bytes in pinyin.dict')


def main():
    os.makedirs(OUT, exist_ok=True)
    write_pinyin()
    write_japanese()


if __name__ == '__main__':
    main()
