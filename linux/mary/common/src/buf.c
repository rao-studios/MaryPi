#include "common/buf.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/secure.h"

int mc_buf_reserve(mc_buf *b, size_t extra) {
    if (extra > SIZE_MAX - b->len - 1) return -ENOMEM;
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) cap = cap > SIZE_MAX / 2 ? need : cap * 2;
    unsigned char *grown = realloc(b->data, cap);
    if (!grown) return -ENOMEM;
    b->data = grown;
    b->cap = cap;
    return 0;
}

int mc_buf_append(mc_buf *b, const void *bytes, size_t n) {
    int rc = mc_buf_reserve(b, n);
    if (rc) return rc;
    if (n) memcpy(b->data + b->len, bytes, n);
    b->len += n;
    b->data[b->len] = 0;
    return 0;
}

int mc_buf_append_str(mc_buf *b, const char *s) { return mc_buf_append(b, s, strlen(s)); }

void mc_buf_consume(mc_buf *b, size_t n) {
    if (n >= b->len) {
        b->len = 0;
    } else {
        memmove(b->data, b->data + n, b->len - n);
        b->len -= n;
    }
    if (b->data) b->data[b->len] = 0;
}

void mc_buf_clear(mc_buf *b) {
    b->len = 0;
    if (b->data) b->data[0] = 0;
}

void mc_buf_free(mc_buf *b) {
    free(b->data);
    *b = (mc_buf){ 0 };
}

void mc_buf_free_secure(mc_buf *b) {
    if (b->data) mc_secure_zero(b->data, b->cap);
    mc_buf_free(b);
}
