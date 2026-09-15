/* Portable ChaCha20 block primitive, also built by host known-answer tests. */
#include <lib/chacha.h>

static uint32_t rotate(uint32_t value, unsigned bits)
{
    return (value << bits) | (value >> (32 - bits));
}
static void quarter(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    *a += *b;
    *d = rotate(*d ^ *a, 16);
    *c += *d;
    *b = rotate(*b ^ *c, 12);
    *a += *b;
    *d = rotate(*d ^ *a, 8);
    *c += *d;
    *b = rotate(*b ^ *c, 7);
}
void chacha20_block(const uint32_t input[16], uint32_t output[16])
{
    uint32_t state[16];
    for (unsigned i = 0; i < 16; i++)
        state[i] = input[i];
    for (unsigned round = 0; round < 10; round++) {
        quarter(&state[0], &state[4], &state[8], &state[12]);
        quarter(&state[1], &state[5], &state[9], &state[13]);
        quarter(&state[2], &state[6], &state[10], &state[14]);
        quarter(&state[3], &state[7], &state[11], &state[15]);
        quarter(&state[0], &state[5], &state[10], &state[15]);
        quarter(&state[1], &state[6], &state[11], &state[12]);
        quarter(&state[2], &state[7], &state[8], &state[13]);
        quarter(&state[3], &state[4], &state[9], &state[14]);
    }
    for (unsigned i = 0; i < 16; i++)
        output[i] = state[i] + input[i];
    /* Volatile stores prevent removal of the temporary-state erase. */
    for (unsigned i = 0; i < 16; i++)
        ((volatile uint32_t *)state)[i] = 0;
}
