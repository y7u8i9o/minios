#!/usr/bin/env python3
"""Write the time zone files of user/share/zoneinfo (L2,
docs/design/time.md).

Each zone of ZONES becomes a TZif version 2 file (RFC 8536) without
transitions, with one local time type and the current POSIX rule of the
zone as the footer.  The C library converts every date of such a zone with
its current rule.  zones.tab lists the zone names and their rules for the
Settings program.  The rules are those of the footers of the IANA time zone
database 2025b."""
import os
import struct
import sys

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(TOP, 'user', 'share', 'zoneinfo')

US = ',M3.2.0,M11.1.0'
EU = ',M3.5.0,M10.5.0/3'
ZONES = [
    ('UTC', 'UTC0'),
    ('Etc/UTC', 'UTC0'),
    ('Africa/Cairo', 'EET-2EEST,M4.5.5/0,M10.5.4/24'),
    ('Africa/Johannesburg', 'SAST-2'),
    ('Africa/Lagos', 'WAT-1'),
    ('Africa/Nairobi', 'EAT-3'),
    ('America/Anchorage', 'AKST9AKDT' + US),
    ('America/Argentina/Buenos_Aires', '<-03>3'),
    ('America/Bogota', '<-05>5'),
    ('America/Chicago', 'CST6CDT' + US),
    ('America/Denver', 'MST7MDT' + US),
    ('America/Lima', '<-05>5'),
    ('America/Los_Angeles', 'PST8PDT' + US),
    ('America/Mexico_City', 'CST6'),
    ('America/New_York', 'EST5EDT' + US),
    ('America/Phoenix', 'MST7'),
    ('America/Santiago', '<-04>4<-03>,M9.1.6/24,M4.1.6/24'),
    ('America/Sao_Paulo', '<-03>3'),
    ('America/Toronto', 'EST5EDT' + US),
    ('America/Vancouver', 'PST8PDT' + US),
    ('Asia/Bangkok', '<+07>-7'),
    ('Asia/Dubai', '<+04>-4'),
    ('Asia/Hong_Kong', 'HKT-8'),
    ('Asia/Jakarta', 'WIB-7'),
    ('Asia/Jerusalem', 'IST-2IDT,M3.4.4/26,M10.5.0'),
    ('Asia/Karachi', 'PKT-5'),
    ('Asia/Kathmandu', '<+0545>-5:45'),
    ('Asia/Kolkata', 'IST-5:30'),
    ('Asia/Seoul', 'KST-9'),
    ('Asia/Shanghai', 'CST-8'),
    ('Asia/Singapore', '<+08>-8'),
    ('Asia/Taipei', 'CST-8'),
    ('Asia/Tehran', '<+0330>-3:30'),
    ('Asia/Tokyo', 'JST-9'),
    ('Australia/Adelaide', 'ACST-9:30ACDT,M10.1.0,M4.1.0/3'),
    ('Australia/Brisbane', 'AEST-10'),
    ('Australia/Melbourne', 'AEST-10AEDT,M10.1.0,M4.1.0/3'),
    ('Australia/Perth', 'AWST-8'),
    ('Australia/Sydney', 'AEST-10AEDT,M10.1.0,M4.1.0/3'),
    ('Europe/Amsterdam', 'CET-1CEST' + EU),
    ('Europe/Athens', 'EET-2EEST,M3.5.0/3,M10.5.0/4'),
    ('Europe/Berlin', 'CET-1CEST' + EU),
    ('Europe/Brussels', 'CET-1CEST' + EU),
    ('Europe/Bucharest', 'EET-2EEST,M3.5.0/3,M10.5.0/4'),
    ('Europe/Dublin', 'IST-1GMT0,M10.5.0,M3.5.0/1'),
    ('Europe/Helsinki', 'EET-2EEST,M3.5.0/3,M10.5.0/4'),
    ('Europe/Istanbul', '<+03>-3'),
    ('Europe/Kyiv', 'EET-2EEST,M3.5.0/3,M10.5.0/4'),
    ('Europe/Lisbon', 'WET0WEST,M3.5.0/1,M10.5.0'),
    ('Europe/London', 'GMT0BST,M3.5.0/1,M10.5.0'),
    ('Europe/Madrid', 'CET-1CEST' + EU),
    ('Europe/Moscow', 'MSK-3'),
    ('Europe/Paris', 'CET-1CEST' + EU),
    ('Europe/Prague', 'CET-1CEST' + EU),
    ('Europe/Rome', 'CET-1CEST' + EU),
    ('Europe/Stockholm', 'CET-1CEST' + EU),
    ('Europe/Vienna', 'CET-1CEST' + EU),
    ('Europe/Warsaw', 'CET-1CEST' + EU),
    ('Europe/Zurich', 'CET-1CEST' + EU),
    ('Pacific/Auckland', 'NZST-12NZDT,M9.5.0,M4.1.0/3'),
    ('Pacific/Honolulu', 'HST10'),
]


def standard(rule):
    """Return the abbreviation and the offset east of UTC of the standard
    time of a rule."""
    i = 0
    if rule[0] == '<':
        i = rule.index('>') + 1
        name = rule[1:i - 1]
    else:
        while i < len(rule) and rule[i].isalpha():
            i += 1
        name = rule[:i]
    j = i
    while j < len(rule) and (rule[j] in '+-:' or rule[j].isdigit()):
        j += 1
    parts = rule[i:j].lstrip('+')
    sign = -1 if parts.startswith('-') else 1
    fields = [int(x) for x in parts.lstrip('-').split(':')] + [0, 0]
    west = sign * (fields[0] * 3600 + fields[1] * 60 + fields[2])
    return name, -west


def tzif(rule):
    name, off = standard(rule)
    chars = name.encode() + b'\0'
    counts = struct.pack('>6l', 0, 0, 0, 0, 1, len(chars))
    header = lambda version: b'TZif' + version + b'\0' * 15 + counts
    data = struct.pack('>lBB', off, 0, 0) + chars
    return header(b'2') + data + header(b'2') + data + b'\n' + rule.encode() + b'\n'


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else OUT
    for name, rule in ZONES:
        path = os.path.join(out, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(tzif(rule))
    with open(os.path.join(out, 'zones.tab'), 'w') as f:
        f.write('# Zone names and their POSIX rules, written by tools/genzoneinfo.py.\n')
        for name, rule in ZONES:
            f.write(f'{name}\t{rule}\n')
    print(f'genzoneinfo: wrote {len(ZONES)} zones into {out}')


if __name__ == '__main__':
    main()
