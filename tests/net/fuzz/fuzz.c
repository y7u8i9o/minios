/* Reproducible mutation driver for the exact validators linked into MiniOS. */
#include <net/wire.h>
#include <net/byteorder.h>
#include <net/checksum.h>
#include <ipc/socket_validate.h>
#include <lib/chacha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

static uint64_t state;
static uint32_t next(void)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}
static void known_answer(void)
{
    uint32_t input[16] = {
        0x61707865,
        0x3320646e,
        0x79622d32,
        0x6b206574,
        0x03020100,
        0x07060504,
        0x0b0a0908,
        0x0f0e0d0c,
        0x13121110,
        0x17161514,
        0x1b1a1918,
        0x1f1e1d1c,
        1,
        0x09000000,
        0x4a000000,
        0,
    };
    static const uint32_t expected[16] = {
        0xe4e7f110,
        0x15593bd1,
        0x1fdd0f50,
        0xc47120a3,
        0xc7f4d1c7,
        0x0368c033,
        0x9aaa2204,
        0x4e6cd4c3,
        0x466482d2,
        0x09aa9f07,
        0x05d7c214,
        0xa2028bd9,
        0xd19c12b5,
        0xb94e16de,
        0xe883d0cb,
        0x4e3c50a2,
    };
    uint32_t output[16];
    chacha20_block(input, output);
    assert(!memcmp(output, expected, sizeof output));
}
int main(int argc, char **argv)
{
    uint64_t seed = argc > 1 ? strtoull(argv[1], NULL, 0) : 0x7090809;
    unsigned seconds = argc > 2 ? strtoul(argv[2], NULL, 0) : 30;
    state = seed ? seed : 1;
    known_answer();
    unsigned long iterations = 0, accepted_tcp = 0, accepted_ip = 0;
    time_t start = time(NULL);
    do {
        size_t length = next() % 512;
        uint8_t *bytes = malloc(length ? length : 1);
        assert(bytes);
        for (size_t i = 0; i < length; i++)
            bytes[i] = next();
        if (length >= 40 && (iterations & 1)) {
            bytes[0] = 0x45;
            bytes[8] = 64;
            bytes[9] = 6;
            net_put_be16(bytes + 2, length);
            net_put_be16(bytes + 6, 0x4000);
            bytes[32] = (5 + next() % 11) << 4;
            bytes[33] = 2;
            net_put_be16(bytes + 36, 0);
            net_put_be16(
                bytes + 36,
                tcp_checksum(
                    net_get_be32(bytes + 12), net_get_be32(bytes + 16), bytes + 20, length - 20));
            net_put_be16(bytes + 10, 0);
            net_put_be16(bytes + 10, net_checksum(bytes, 20));
        }
        struct tcp_segment segment;
        if (tcp_parse_segment(bytes, length, &segment)) {
            assert(segment.data >= bytes + 40 && segment.data + segment.length == bytes + length);
            accepted_tcp++;
        }
        unsigned total = 0, offset = 0;
        if (ipv4_header_valid(bytes, length, &total)) {
            assert(total <= length && total >= 20);
            accepted_ip++;
        }
        ipv4_fragment_bounds(next(), iterations % 8 ? length : SIZE_MAX, 8108, &offset);
        udp_wire_valid(next(), next(), bytes, length);
        socket_parse_address(bytes, length);
        size_t lengths[32], sum = 0;
        for (unsigned i = 0; i < 32; i++)
            lengths[i] = iterations % 8 ? next() % 65536 : SIZE_MAX - next();
        size_t count = next() % 34;
        int result = socket_iovec_size(lengths, count, &sum);
        if (!result) {
            assert(count <= 32 && sum <= ((size_t)1 << 30));
            size_t reference = 0;
            for (size_t i = 0; i < count; i++)
                reference += lengths[i];
            assert(sum == reference);
        }
        free(bytes);
        iterations++;
    } while (time(NULL) - start < seconds);
    printf("seed=0x%llx seconds=%ld iterations=%lu tcp_valid=%lu ip_valid=%lu KAT=pass\n",
           (unsigned long long)seed,
           (long)(time(NULL) - start),
           iterations,
           accepted_tcp,
           accepted_ip);
    return 0;
}
