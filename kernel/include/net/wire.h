#pragma once
/* Pure wire validators shared by kernel input and host sanitizer fuzzing. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct tcp_segment {
    uint32_t source, destination;
    uint16_t source_port, destination_port;
    uint32_t sequence, acknowledgement;
    uint16_t window, mss;
    uint8_t flags;
    const uint8_t *data;
    size_t length;
};

bool tcp_parse_segment(const uint8_t *packet, size_t length, struct tcp_segment *segment);
uint16_t tcp_checksum(uint32_t source, uint32_t destination, const void *data, size_t length);
bool ipv4_header_valid(const uint8_t *packet, size_t length, unsigned *total);
bool ipv4_fragment_bounds(unsigned flags, size_t length, unsigned maximum, unsigned *offset);
bool udp_wire_valid(uint32_t source, uint32_t destination, const uint8_t *header, size_t length);
