#!/usr/bin/env python3
"""Independently verify IPv4 fragments and transport checksums in QEMU PCAP."""
import struct
import sys


def checksum(data):
    data += b'\0' * (len(data) % 2)
    value = sum(struct.unpack('!' + 'H' * (len(data) // 2), data))
    while value >> 16:
        value = (value & 65535) + (value >> 16)
    return (~value) & 65535


counts = {'arp': 0, 'icmp': 0, 'udp': 0, 'tcp': 0, 'fragments': 0}
fragments = {}


def transport(ip, payload):
    protocol = ip[9]
    if protocol == 1:
        assert checksum(payload) == 0, 'ICMP checksum'
        counts['icmp'] += 1
    elif protocol == 17:
        assert len(payload) >= 8, 'UDP minimum header'
        assert int.from_bytes(payload[4:6], 'big') == len(payload), 'UDP length'
        if payload[6:8] != b'\0\0':
            pseudo = ip[12:20] + bytes([0, 17]) + len(payload).to_bytes(2, 'big')
            assert checksum(pseudo + payload) == 0, 'UDP checksum'
        counts['udp'] += 1
    elif protocol == 6:
        assert len(payload) >= 20, 'TCP minimum header'
        assert 20 <= (payload[12] >> 4) * 4 <= len(payload), 'TCP data offset'
        pseudo = ip[12:20] + bytes([0, 6]) + len(payload).to_bytes(2, 'big')
        assert checksum(pseudo + payload) == 0, 'TCP checksum'
        counts['tcp'] += 1


with open(sys.argv[1], 'rb') as file:
    magic = file.read(24)
    assert magic[:4] in (b'\xd4\xc3\xb2\xa1', b'\xa1\xb2\xc3\xd4'), 'pcap magic'
    endian = '<' if magic[0] == 0xd4 else '>'
    while header := file.read(16):
        _, _, size, original = struct.unpack(endian + 'IIII', header)
        data = file.read(size)
        assert len(data) == size == original and size >= 14, 'frame length'
        ethertype = data[12:14]
        if ethertype == b'\x08\x06':
            counts['arp'] += 1
        if ethertype != b'\x08\x00':
            continue
        ip = data[14:]
        ihl = (ip[0] & 15) * 4
        total = int.from_bytes(ip[2:4], 'big')
        assert ihl >= 20 and ihl <= total <= len(ip), 'IPv4 length'
        assert checksum(ip[:ihl]) == 0, 'IPv4 checksum'
        payload = ip[ihl:total]
        flags = int.from_bytes(ip[6:8], 'big')
        if not flags & 0x3fff:
            transport(ip, payload)
            continue
        counts['fragments'] += 1
        offset = (flags & 8191) * 8
        more = bool(flags & 0x2000)
        assert not flags & 0x4000 and payload, 'fragment flags or empty payload'
        assert not more or len(payload) % 8 == 0, 'fragment alignment'
        key = (ip[12:20], ip[4:6], ip[9])
        entry = fragments.setdefault(key, {'pieces': {}, 'end': None, 'ip': ip})
        end = offset + len(payload)
        if not more:
            assert entry['end'] in (None, end), 'conflicting fragment end'
            entry['end'] = end
        for start, piece in entry['pieces'].items():
            if offset < start + len(piece) and start < end:
                assert start == offset and piece == payload, 'overlap'
        entry['pieces'][offset] = payload
        cursor = 0
        assembled = bytearray()
        for start, piece in sorted(entry['pieces'].items()):
            if start != cursor:
                break
            assembled += piece
            cursor += len(piece)
        if entry['end'] == cursor:
            transport(entry['ip'], bytes(assembled))
            del fragments[key]
    assert not fragments, 'incomplete captured datagrams'
    assert counts['arp'] >= 2 and sum(counts[k] for k in ('icmp', 'udp', 'tcp')) >= 2, counts
    print('capture checks passed:', counts)
