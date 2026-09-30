#include <net/wire.h>
#include <net/byteorder.h>
#include <net/checksum.h>

uint16_t tcp_checksum(uint32_t source, uint32_t destination, const void *data, size_t length)
{
    uint8_t pseudo[12];
    net_put_be32(pseudo, source);
    net_put_be32(pseudo + 4, destination);
    pseudo[8] = 0;
    pseudo[9] = 6;
    net_put_be16(pseudo + 10, (uint16_t)length);
    uint32_t sum = net_checksum_partial(pseudo, sizeof pseudo, 0);
    return net_checksum_finish(net_checksum_partial(data, length, sum));
}

bool tcp_parse_segment(const uint8_t *ip, size_t packet_length, struct tcp_segment *segment)
{
    if (packet_length < 40)
        return false;
    const uint8_t *header = ip + 20;
    size_t length = packet_length - 20;
    size_t header_length = (header[12] >> 4) * 4;
    if (header_length < 20 || header_length > length || (header[12] & 15))
        return false;
    segment->source = net_get_be32(ip + 12);
    segment->destination = net_get_be32(ip + 16);
    if (tcp_checksum(segment->source, segment->destination, header, length))
        return false;
    segment->source_port = net_get_be16(header);
    segment->destination_port = net_get_be16(header + 2);
    segment->sequence = net_get_be32(header + 4);
    segment->acknowledgement = net_get_be32(header + 8);
    segment->flags = header[13];
    segment->window = net_get_be16(header + 14);
    segment->mss = 536;
    segment->has_window_scale = false;
    segment->has_timestamp = false;
    segment->window_scale = 0;
    segment->timestamp_value = 0;
    segment->timestamp_echo = 0;
    segment->data = header + header_length;
    segment->length = length - header_length;
    if (!segment->source_port || !segment->destination_port || (segment->flags & 0x20) ||
        ((segment->flags & 0x02) && (segment->flags & 0x01)))
        return false;

    /* A malformed or repeated option of a kind the stack interprets makes
     * the whole segment invalid; well-framed unknown options are skipped. */
    bool saw_mss = false;
    bool syn = segment->flags & 0x02;
    for (size_t offset = 20; offset < header_length;) {
        uint8_t kind = header[offset];
        if (kind == 0)
            break;
        if (kind == 1) {
            offset++;
            continue;
        }
        if (offset + 2 > header_length)
            return false;
        size_t option_length = header[offset + 1];
        if (option_length < 2 || option_length > header_length - offset)
            return false;
        const uint8_t *value = header + offset + 2;
        if (kind == 2 && syn) {
            if (option_length != 4 || saw_mss)
                return false;
            segment->mss = net_get_be16(value);
            if (!segment->mss)
                return false;
            saw_mss = true;
        } else if (kind == 3 && syn) {
            if (option_length != 3 || segment->has_window_scale)
                return false;
            segment->window_scale = value[0];
            segment->has_window_scale = true;
        } else if (kind == 8) {
            if (option_length != 10 || segment->has_timestamp)
                return false;
            segment->timestamp_value = net_get_be32(value);
            segment->timestamp_echo = net_get_be32(value + 4);
            segment->has_timestamp = true;
        }
        offset += option_length;
    }
    return true;
}

bool ipv4_header_valid(const uint8_t *packet, size_t length, unsigned *total)
{
    if (length < 20 || (packet[0] >> 4) != 4 || (packet[0] & 15) < 5)
        return false;
    unsigned ihl = (packet[0] & 15) * 4;
    unsigned bytes = net_get_be16(packet + 2);
    if (ihl > length || bytes < ihl || bytes > length || !packet[8] || net_checksum(packet, ihl) ||
        (net_get_be16(packet + 6) & 0x8000))
        return false;
    *total = bytes;
    return true;
}

bool ipv4_fragment_bounds(unsigned flags, size_t length, unsigned maximum, unsigned *offset)
{
    unsigned start = (flags & 8191) * 8;
    if ((flags & 0xc000) || !length || ((flags & 0x2000) && length % 8) || start > maximum ||
        length > maximum - start)
        return false;
    *offset = start;
    return true;
}

bool udp_wire_valid(uint32_t source, uint32_t destination, const uint8_t *header, size_t length)
{
    if (length < 8 || length > 65535 || net_get_be16(header + 4) != length ||
        !net_get_be16(header + 2))
        return false;
    if (!net_get_be16(header + 6))
        return true;
    uint8_t pseudo[12];
    net_put_be32(pseudo, source);
    net_put_be32(pseudo + 4, destination);
    pseudo[8] = 0;
    pseudo[9] = 17;
    net_put_be16(pseudo + 10, length);
    return net_checksum_finish(
               net_checksum_partial(header, length, net_checksum_partial(pseudo, 12, 0))) == 0;
}
