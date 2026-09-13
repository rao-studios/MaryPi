/* Two small pieces between PipeWire's real-time thread and maryd, with no locks:
 *
 * mv_ring      a single-producer, single-consumer ring of float samples — maryd
 *              writes the reply's PCM, the playback callback reads it.
 * mv_framer    gathers whatever PipeWire hands the capture callback into the fixed
 *              20 ms frames the VAD and keyword spotter want. */
#ifndef MARY_VOICE_RING_H
#define MARY_VOICE_RING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct mv_ring {
    float *data;
    size_t mask;                /* capacity - 1, a power of two */
    atomic_size_t head;         /* written by the producer */
    atomic_size_t tail;         /* written by the consumer */
    atomic_bool flush;          /* producer asks; the consumer empties the ring */
} mv_ring;

/* Capacity is rounded up to a power of two. 0, or -ENOMEM. */
int mv_ring_init(mv_ring *ring, size_t capacity);
void mv_ring_free(mv_ring *ring);
/* Producer: as many samples as fit. */
size_t mv_ring_write(mv_ring *ring, const float *samples, size_t count);
/* Consumer: up to count samples; honours a pending flush first. */
size_t mv_ring_read(mv_ring *ring, float *out, size_t count);
/* Producer: drop everything queued (the consumer does it on its next read). */
void mv_ring_request_flush(mv_ring *ring);
size_t mv_ring_available(const mv_ring *ring);
size_t mv_ring_capacity(const mv_ring *ring);

typedef void (*mv_frame_fn)(const int16_t *frame, size_t count, void *user);

typedef struct mv_framer {
    int16_t *frame;
    size_t size;                /* samples per frame: 320 at 16 kHz is 20 ms */
    size_t filled;
} mv_framer;

int mv_framer_init(mv_framer *framer, size_t frame_samples);
void mv_framer_free(mv_framer *framer);
/* Feeds samples; calls fn once per whole frame. */
void mv_framer_push(mv_framer *framer, const int16_t *samples, size_t count, mv_frame_fn fn, void *user);
void mv_framer_reset(mv_framer *framer);

#endif
