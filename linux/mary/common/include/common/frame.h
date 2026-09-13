/* sewnd's socket framing: a 5-byte header — the payload length as a
 * little-endian u32, then a kind byte — followed by the payload. JSON frames
 * carry Sewn's realtime wire messages; PCM frames carry audio, their order on
 * the socket being the audio's order. One writer per socket. */
#ifndef MARY_COMMON_FRAME_H
#define MARY_COMMON_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/buf.h"

#define MC_FRAME_HEADER 5
#define MC_FRAME_MAX (1u << 20)     /* Sewn's realtime route caps frames at 1 MiB too */

enum mc_frame_kind {
    MC_FRAME_JSON = 1,
    MC_FRAME_PCM = 2,
};

/* `payload` is valid only during the call. Return nonzero to stop. */
typedef int (*mc_frame_fn)(uint8_t kind, const unsigned char *payload, size_t len, void *user);

typedef struct mc_frame_reader {
    mc_buf buf;
    size_t max_payload;
    bool secure;            /* zero bytes once consumed: the connection may carry the API key */
} mc_frame_reader;

/* max_payload 0 means MC_FRAME_MAX. */
void mc_frame_reader_init(mc_frame_reader *r, size_t max_payload, bool secure);
/* 0; 1 when fn stopped; -EMSGSIZE when a header announces more than
 * max_payload (the stream cannot be trusted after that: close it); -ENOMEM. */
int mc_frame_reader_feed(mc_frame_reader *r, const unsigned char *bytes, size_t n, mc_frame_fn fn, void *user);
/* One read(2) from fd, fed through: MC_IO_OK, MC_IO_STOPPED, MC_IO_EOF, or -errno
 * (-EAGAIN when a non-blocking fd has nothing). */
int mc_frame_reader_read_fd(mc_frame_reader *r, int fd, mc_frame_fn fn, void *user);
void mc_frame_reader_free(mc_frame_reader *r);

/* Appends one frame to `out`. 0, -EMSGSIZE, or -ENOMEM. */
int mc_frame_append(mc_buf *out, uint8_t kind, const unsigned char *payload, size_t len);
/* Writes one whole frame to a blocking fd. 0, or -errno. */
int mc_frame_write_fd(int fd, uint8_t kind, const unsigned char *payload, size_t len);

struct json_object;
/* A JSON frame holding `obj`, serialized compactly. 0, or -errno. */
int mc_frame_write_json(int fd, struct json_object *obj);

#endif
