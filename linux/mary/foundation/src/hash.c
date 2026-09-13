#include "foundation/hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/sha256.h"

uint64_t mf_fnv1a64(const char *s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 0x100000001b3ull;
    }
    return h;
}

void mf_fnv1a64_hex(const char *s, char out[17]) {
    snprintf(out, 17, "%016llx", (unsigned long long)mf_fnv1a64(s));
}

int64_t mf_djb2(const char *s) {
    uint64_t h = 5381;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) h = h * 33 + *p;
    return (int64_t)h;
}

void mf_numeric_hash(const void *bytes, size_t len, char out[MF_NUMERIC_HASH_MAX]) {
    unsigned char digest[MC_SHA256_BYTES];
    mc_sha256(bytes, len, digest);
    size_t n = 0;
    for (int i = 0; i < MC_SHA256_BYTES; i++) n += (size_t)snprintf(out + n, (size_t)MF_NUMERIC_HASH_MAX - n, "%02d", digest[i]);
    out[n] = 0;
}

void mf_numeric_hash_text(const char *text, char out[MF_NUMERIC_HASH_MAX]) {
    mf_numeric_hash(text, strlen(text), out);
}

/* Whitespace as Swift's Character.isWhitespace sees the common cases: ASCII, NBSP,
 * the line and paragraph separators, the ideographic space. Returns the bytes
 * consumed, 0 when `p` does not start whitespace. */
static size_t whitespace_len(const unsigned char *p) {
    if (*p == ' ' || (*p >= 0x09 && *p <= 0x0D)) return 1;
    if (p[0] == 0xC2 && p[1] == 0xA0) return 2;                                   /* U+00A0 */
    if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0xA8 || p[2] == 0xA9)) return 3;  /* U+2028, U+2029 */
    if (p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x80) return 3;                    /* U+3000 */
    return 0;
}

char *mf_canonical(const char *s) {
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    size_t w = 0;
    int pending_space = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        size_t ws = whitespace_len(p);
        if (ws) {
            if (w) pending_space = 1;
            p += ws;
            continue;
        }
        if (pending_space) {
            out[w++] = ' ';
            pending_space = 0;
        }
        if (*p >= 'A' && *p <= 'Z') {
            out[w++] = (char)(*p + 32);
            p++;
        } else if (p[0] == 0xC3 && p[1] >= 0x80 && p[1] <= 0x9E && p[1] != 0x97) {  /* À–Þ except × */
            out[w++] = (char)0xC3;
            out[w++] = (char)(p[1] + 0x20);
            p += 2;
        } else {
            out[w++] = (char)*p++;
        }
    }
    out[w] = 0;
    return out;
}
