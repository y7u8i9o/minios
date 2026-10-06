/* Boot-seeded generator with key erasure after each output. The host entropy
 * source is a trust boundary. There is no claim of recovery after compromise
 * of the live generator state without a new boot seed. */
#define KLOG_SUBSYS "random"
#include <lib/random.h>
#include <lib/chacha.h>
#include <drivers/virtio/virtio_rng.h>
#include <sync/spinlock.h>
#include <klog.h>
#include <errno.h>
#include <lib/string.h>

static DEFINE_SPINLOCK(random_lock);
static bool ready;
static uint32_t state[16];
static uint32_t little_word(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
           (uint32_t)bytes[3] << 24;
}
void random_init(void)
{
    uint8_t seed[64] = {0};
    if (virtio_rng_seed(seed) < 0) {
        klog_warn("entropy unavailable, automatic Internet ports and TCP opens disabled");
        return;
    }
    bool varied = false;
    for (unsigned i = 1; i < sizeof seed; i++)
        varied |= seed[i] != seed[0];
    if (varied) {
        state[0] = 0x61707865;
        state[1] = 0x3320646e;
        state[2] = 0x79622d32;
        state[3] = 0x6b206574;
        for (unsigned i = 0; i < 8; i++)
            state[4 + i] = little_word(seed + i * 4);
        for (unsigned i = 0; i < 3; i++)
            state[13 + i] = little_word(seed + 32 + i * 4);
        __atomic_store_n(&ready, true, __ATOMIC_RELEASE);
        klog_info("ready, VirtIO boot seed with ChaCha20 key erasure");
    } else {
        klog_warn("constant entropy rejected");
    }
    for (unsigned i = 0; i < sizeof seed; i++)
        ((volatile uint8_t *)seed)[i] = 0;
}
bool random_ready(void)
{
    return __atomic_load_n(&ready, __ATOMIC_ACQUIRE);
}
int random_bytes(void *buf, size_t n)
{
    if (!random_ready())
        return -EAGAIN;
    uint8_t *out = buf;
    uint32_t block[16];
    spin_lock(&random_lock);
    while (n) {
        chacha20_block(state, block);
        /* The first half of each block becomes the new key, the second
         * half is output: output never reveals a key that was used. */
        for (unsigned i = 0; i < 8; i++)
            state[4 + i] = block[i];
        state[12]++;
        size_t k = n < 32 ? n : 32;
        memcpy(out, block + 8, k);
        out += k;
        n -= k;
    }
    for (unsigned i = 0; i < 16; i++)
        ((volatile uint32_t *)block)[i] = 0;
    spin_unlock(&random_lock);
    return 0;
}

int random_u32(uint32_t *value)
{
    if (!random_ready())
        return -EAGAIN;
    uint32_t block[16];
    spin_lock(&random_lock);
    chacha20_block(state, block);
    for (unsigned i = 0; i < 8; i++)
        state[4 + i] = block[i];
    /* Re-keying makes the next block distinct even across counter wrap. */
    state[12]++;
    *value = block[8];
    for (unsigned i = 0; i < 16; i++)
        ((volatile uint32_t *)block)[i] = 0;
    spin_unlock(&random_lock);
    return 0;
}
