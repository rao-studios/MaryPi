/* RFC 4648 base64 with the standard alphabet. Decoding accepts padded or
 * unpadded input and rejects anything else (whitespace included). */
#ifndef MARY_COMMON_BASE64_H
#define MARY_COMMON_BASE64_H

#include <stddef.h>

/* Characters in the encoding of n bytes, without the NUL. */
size_t mc_base64_encoded_len(size_t n);
/* An upper bound on the bytes n characters decode to. */
size_t mc_base64_decoded_max(size_t n);

/* Writes the encoding and a NUL. 0, or -ENOSPC when cap < encoded_len + 1. */
int mc_base64_encode(const unsigned char *in, size_t n, char *out, size_t cap, size_t *written);
/* 0, -EINVAL for malformed input, or -ENOSPC. */
int mc_base64_decode(const char *in, size_t n, unsigned char *out, size_t cap, size_t *written);

/* Heap versions: NULL on failure (errno says why); the caller frees. */
char *mc_base64_encode_alloc(const unsigned char *in, size_t n, size_t *len);
unsigned char *mc_base64_decode_alloc(const char *in, size_t n, size_t *len);

#endif
