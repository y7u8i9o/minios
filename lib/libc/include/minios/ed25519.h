#pragma once
/* This header declares Ed25519 signatures (RFC 8032, section 5.1). The
 * package installer verifies repository indexes with ed25519_verify, and
 * tools/pkgsign derives keys and signs on the host with the same source. */
#include <stddef.h>
#include <stdint.h>

#define ED25519_SEED_SIZE 32
#define ED25519_PUBLIC_SIZE 32
#define ED25519_SIGNATURE_SIZE 64

/* ed25519_public_key writes the public key of a 32 byte secret seed. */
void ed25519_public_key(uint8_t pub[ED25519_PUBLIC_SIZE], const uint8_t seed[ED25519_SEED_SIZE]);
/* ed25519_sign writes the signature of len bytes at msg under the key
 * of seed. */
void ed25519_sign(uint8_t sig[ED25519_SIGNATURE_SIZE], const void *msg, size_t len,
                  const uint8_t seed[ED25519_SEED_SIZE]);
/* ed25519_verify returns 1 when sig is a valid signature of msg under
 * pub and 0 otherwise. A public key that does not decode to a curve point
 * and a signature whose scalar is not below the group order are invalid. */
int ed25519_verify(const uint8_t sig[ED25519_SIGNATURE_SIZE], const void *msg, size_t len,
                   const uint8_t pub[ED25519_PUBLIC_SIZE]);
