#include "common/base64.h"

#include <errno.h>
#include <stdlib.h>

static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t mc_base64_encoded_len(size_t n) { return 4 * ((n + 2) / 3); }
size_t mc_base64_decoded_max(size_t n) { return n / 4 * 3 + 3; }

int mc_base64_encode(const unsigned char *in, size_t n, char *out, size_t cap, size_t *written) {
    size_t need = mc_base64_encoded_len(n);
    if (need + 1 > cap) return -ENOSPC;
    size_t o = 0, i = 0;
    for (; i + 2 < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8 | in[i + 2];
        out[o++] = ALPHABET[v >> 18 & 63];
        out[o++] = ALPHABET[v >> 12 & 63];
        out[o++] = ALPHABET[v >> 6 & 63];
        out[o++] = ALPHABET[v & 63];
    }
    if (i < n) {
        unsigned v = (unsigned)in[i] << 16 | (i + 1 < n ? (unsigned)in[i + 1] << 8 : 0);
        out[o++] = ALPHABET[v >> 18 & 63];
        out[o++] = ALPHABET[v >> 12 & 63];
        out[o++] = i + 1 < n ? ALPHABET[v >> 6 & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    if (written) *written = o;
    return 0;
}

static int value_of(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int mc_base64_decode(const char *in, size_t n, unsigned char *out, size_t cap, size_t *written) {
    size_t len = n;
    if (len && in[len - 1] == '=') {
        if (n % 4 != 0) return -EINVAL;
        len--;
        if (len && in[len - 1] == '=') len--;
    }
    if (len % 4 == 1) return -EINVAL;
    size_t need = len / 4 * 3 + (len % 4 ? len % 4 - 1 : 0);
    if (need > cap) return -ENOSPC;
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = value_of((unsigned char)in[i]);
        if (v < 0) return -EINVAL;
        acc = acc << 6 | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)(acc >> bits);
            acc &= (1u << bits) - 1;
        }
    }
    if (written) *written = o;
    return 0;
}

char *mc_base64_encode_alloc(const unsigned char *in, size_t n, size_t *len) {
    size_t cap = mc_base64_encoded_len(n) + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    mc_base64_encode(in, n, out, cap, len);
    return out;
}

unsigned char *mc_base64_decode_alloc(const char *in, size_t n, size_t *len) {
    size_t cap = mc_base64_decoded_max(n) + 1;
    unsigned char *out = malloc(cap);
    if (!out) return NULL;
    int rc = mc_base64_decode(in, n, out, cap, len);
    if (rc < 0) {
        free(out);
        errno = -rc;
        return NULL;
    }
    return out;
}
