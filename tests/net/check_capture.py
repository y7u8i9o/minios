#!/usr/bin/env python3
"""Independently verify IPv4 fragments, transport checksums and TCP option
use in QEMU PCAP.

usage: check_capture.py [--require FEATURE]... CAPTURE

Every TCP connection in the capture is checked against the options its
two SYN segments carried (RFC 7323): timestamps appear on every later
segment exactly when both SYNs carried them and each echo repeats a value
the other side sent; when both SYNs carried window scaling, no data
segment ends beyond the right edge the receiver last advertised with its
scale applied; SACK blocks appear only when both SYNs carried
SACK-permitted and each block lies inside the sequence space the other
side has sent. --require names a feature the capture must show:
window-scale (a negotiated connection sent data beyond the edge that an
unscaled reading of the window would give), timestamps (a connection
negotiated them), sack (both sides of a connection sent SACK blocks),
fallback (a SYN offered options that the other SYN did not, and neither
side used them afterwards), arp-probe (an ARP request with sender address
0, RFC 5227) and arp-announce (an ARP request whose sender and target
addresses are equal and nonzero).
"""
import struct
import sys


def checksum(data):
    data += b'\0' * (len(data) % 2)
    value = sum(struct.unpack('!' + 'H' * (len(data) // 2), data))
    while value >> 16:
        value = (value & 65535) + (value >> 16)
    return (~value) & 65535


counts = {'arp': 0, 'icmp': 0, 'udp': 0, 'tcp': 0, 'fragments': 0,
          'tcp_window_scale': 0, 'tcp_timestamps': 0, 'tcp_fallback': 0,
          'scaled_window_use': 0, 'tcp_sack': 0, 'sack_blocks': 0,
          'arp_probe': 0, 'arp_announce': 0}
fragments = {}
connections = {}


def tcp_options(header):
    """This function returns the options of one TCP header as a dictionary."""
    found = {}
    offset = 20
    end = (header[12] >> 4) * 4
    while offset < end:
        kind = header[offset]
        if kind == 0:
            break
        if kind == 1:
            offset += 1
            continue
        assert offset + 2 <= end and 2 <= header[offset + 1] <= end - offset, 'TCP option framing'
        length = header[offset + 1]
        value = header[offset + 2:offset + length]
        if kind == 3:
            assert length == 3, 'window scale length'
            found['wscale'] = value[0]
        elif kind == 8:
            assert length == 10, 'timestamp length'
            found['ts'] = struct.unpack('!II', value)
        elif kind == 4:
            assert length == 2, 'SACK-permitted length'
            found['sackok'] = True
        elif kind == 5:
            assert length >= 10 and (length - 2) % 8 == 0 and length <= 34, 'SACK length'
            found['sack'] = [struct.unpack('!II', value[i:i + 8]) for i in range(0, length - 2, 8)]
        offset += length
    return found


def tcp_connection(ip, payload):
    """This function applies the option consistency rules to one TCP segment."""
    source = (ip[12:16], payload[0:2])
    destination = (ip[16:20], payload[2:4])
    key = tuple(sorted((source, destination)))
    flags = payload[13]
    options = tcp_options(payload)
    state = connections.setdefault(key, {'syn': {}, 'tsvals': {}, 'edge': {}, 'counted': False,
                                         'high': {}, 'sackers': set()})
    sequence = struct.unpack('!I', payload[4:8])[0]
    length = len(payload) - (payload[12] >> 4) * 4
    end = (sequence + length + ((flags & 0x03) != 0)) & 0xffffffff
    high = state['high'].get(source)
    if high is None or 0 < ((end - high) & 0xffffffff) < 0x80000000:
        state['high'][source] = end
    if flags & 0x02:
        state['syn'][source] = options
        if 'ts' in options:
            state['tsvals'].setdefault(source, set()).add(options['ts'][0])
        return
    syns = state['syn']
    if source not in syns or destination not in syns:
        return
    mine, theirs = syns[source], syns[destination]
    timestamps = 'ts' in mine and 'ts' in theirs
    scaling = 'wscale' in mine and 'wscale' in theirs
    sack = 'sackok' in mine and 'sackok' in theirs
    if not state['counted']:
        state['counted'] = True
        counts['tcp_timestamps'] += timestamps
        counts['tcp_window_scale'] += scaling
        counts['tcp_sack'] += sack
        offered = set(k for k in ('ts', 'wscale', 'sackok') if k in mine or k in theirs)
        if offered and not timestamps and not scaling and not sack:
            counts['tcp_fallback'] += 1
    if 'sack' in options:
        assert sack, 'SACK blocks on a connection that did not negotiate SACK'
        peer_high = state['high'].get(destination)
        for left, right in options['sack']:
            assert 0 < ((right - left) & 0xffffffff) < 0x80000000, 'empty or reversed SACK block'
            assert ((peer_high - right) & 0xffffffff) < 0x80000000, 'SACK block beyond sent data'
        counts['sack_blocks'] += len(options['sack'])
        state['sackers'].add(source)
        if len(state['sackers']) == 2:
            counts['sack_both_directions'] = 1
    if timestamps and not flags & 0x04:
        assert 'ts' in options, 'timestamp missing on a connection that negotiated it'
    if not timestamps:
        assert 'ts' not in options, 'timestamp on a connection that did not negotiate it'
    if 'ts' in options:
        value, echo = options['ts']
        state['tsvals'].setdefault(source, set()).add(value)
        if flags & 0x10:
            assert echo in state['tsvals'].get(destination, set()), 'timestamp echo never sent'
    acknowledgement = struct.unpack('!I', payload[8:12])[0]
    window = int.from_bytes(payload[14:16], 'big')
    if scaling:
        shift = min(mine['wscale'], 14)
        if flags & 0x10:
            edge = (acknowledgement + (window << shift)) & 0xffffffff
            state['edge'][source] = (acknowledgement, window, edge)
        if length and destination in state['edge']:
            ack, raw, edge = state['edge'][destination]
            end = (sequence + length) & 0xffffffff
            assert ((edge - end) & 0xffffffff) < 0x80000000, 'data beyond the scaled window'
            if 0 < ((end - (ack + raw)) & 0xffffffff) < 0x80000000:
                counts['scaled_window_use'] += 1


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
        tcp_connection(ip, payload)


arguments = sys.argv[1:]
required = []
while arguments and arguments[0] == '--require':
    required.append(arguments[1])
    arguments = arguments[2:]
assert len(arguments) == 1, 'usage: check_capture.py [--require FEATURE]... CAPTURE'

with open(arguments[0], 'rb') as file:
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
            arp = data[14:42]
            assert len(arp) == 28, 'ARP length'
            if arp[6:8] == b'\x00\x01' and arp[14:18] == b'\0\0\0\0':
                assert arp[24:28] != b'\0\0\0\0' and arp[18:24] == b'\0' * 6, 'ARP probe fields'
                counts['arp_probe'] += 1
            elif arp[6:8] == b'\x00\x01' and arp[14:18] == arp[24:28]:
                counts['arp_announce'] += 1
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
    for feature in required:
        if feature == 'window-scale':
            assert counts['tcp_window_scale'] and counts['scaled_window_use'], counts
        elif feature == 'timestamps':
            assert counts['tcp_timestamps'], counts
        elif feature == 'sack':
            assert counts.get('sack_both_directions'), counts
        elif feature in ('arp-probe', 'arp-announce'):
            assert counts[feature.replace('-', '_')], counts
        elif feature == 'fallback':
            assert counts['tcp_fallback'], counts
        else:
            raise AssertionError('unknown feature ' + feature)
    print('capture checks passed:', counts)
