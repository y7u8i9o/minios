/* Huffman decoding for AAC. The codebooks of ISO/IEC 14496-3 are not
 * canonical. The decoder therefore builds a binary tree from the code and
 * the length of every symbol and walks the tree one bit at a time. The
 * longest code has 19 bits. */
#include "aac.h"
#include <stdlib.h>

int aac_huffman_build(struct aac_huffman *h, const struct aac_codebook *cb)
{
    /* A complete prefix code of n symbols has n - 1 internal nodes. */
    h->node = calloc(cb->size, sizeof *h->node);
    if (!h->node)
        return -ENOMEM;
    h->count = 1;
    for (unsigned sym = 0; sym < cb->size; sym++) {
        uint32_t code = cb->code32 ? cb->code32[sym] : cb->code16[sym];
        unsigned len = cb->bits[sym], at = 0;
        for (unsigned i = len; i-- > 0;) {
            unsigned bit = (code >> i) & 1;
            if (i == 0) {
                if (h->node[at][bit])
                    goto invalid;
                h->node[at][bit] = (int16_t)-(int)(sym + 1);
                break;
            }
            int16_t next = h->node[at][bit];
            if (next < 0)
                goto invalid;
            if (next == 0) {
                if (h->count >= cb->size)
                    goto invalid;
                next = (int16_t)h->count++;
                h->node[at][bit] = next;
            }
            at = (unsigned)next;
        }
    }
    return 0;
invalid:
    aac_huffman_free(h);
    return -EINVAL;
}

void aac_huffman_free(struct aac_huffman *h)
{
    free(h->node);
    h->node = NULL;
    h->count = 0;
}

int aac_huffman_decode(const struct aac_huffman *h, struct aac_bits *b)
{
    unsigned at = 0;
    for (unsigned depth = 0; depth < 32; depth++) {
        int16_t next = h->node[at][aac_bit(b)];
        if (b->overrun || next == 0)
            return -1;
        if (next < 0)
            return -next - 1;
        at = (unsigned)next;
    }
    return -1;
}
