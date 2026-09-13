/* A growable byte buffer. `data` always has a NUL after `len` bytes, so text can
 * be read as a C string; `cap` counts that byte. A zeroed mc_buf is empty. */
#ifndef MARY_COMMON_BUF_H
#define MARY_COMMON_BUF_H

#include <stddef.h>

typedef struct mc_buf {
    unsigned char *data;
    size_t len;
    size_t cap;
} mc_buf;

/* Room for `extra` more bytes. 0, or -ENOMEM. */
int mc_buf_reserve(mc_buf *b, size_t extra);
/* 0, or -ENOMEM. */
int mc_buf_append(mc_buf *b, const void *bytes, size_t n);
int mc_buf_append_str(mc_buf *b, const char *s);
/* Drops the first n bytes (all of them when n >= len). */
void mc_buf_consume(mc_buf *b, size_t n);
void mc_buf_clear(mc_buf *b);
void mc_buf_free(mc_buf *b);
/* Zeroes the whole allocation before freeing it: for buffers that held a secret. */
void mc_buf_free_secure(mc_buf *b);

#endif
