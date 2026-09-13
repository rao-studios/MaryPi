/* SHA-256 (FIPS 180-4), the way swift-crypto's `SHA256.hash` reads: Thread's
 * document, partition, entity and relationship ids are SHA-256 digests, so the
 * hard drive needs the function with no library behind it. */
#ifndef MARY_COMMON_SHA256_H
#define MARY_COMMON_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define MC_SHA256_BYTES 32

typedef struct mc_sha256_ctx {
    uint32_t state[8];
    uint64_t length;            /* bytes so far */
    unsigned char block[64];
    size_t block_len;
} mc_sha256_ctx;

void mc_sha256_init(mc_sha256_ctx *c);
void mc_sha256_update(mc_sha256_ctx *c, const void *data, size_t len);
void mc_sha256_final(mc_sha256_ctx *c, unsigned char out[MC_SHA256_BYTES]);
/* One shot. */
void mc_sha256(const void *data, size_t len, unsigned char out[MC_SHA256_BYTES]);
/* Lowercase hex, NUL-terminated (65 bytes). */
void mc_sha256_hex(const unsigned char digest[MC_SHA256_BYTES], char out[65]);

#endif
