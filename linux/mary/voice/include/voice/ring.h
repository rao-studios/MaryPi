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
    atomic_size_t flush_to;     /* producer: drop what was queued before this head; SIZE_MAX: none */
} mv_ring;

/* Capacity is rounded up to a power of two. 0, or -ENOMEM. */
int mv_ring_init(mv_ring *ring, size_t capacity);
void mv_ring_free(mv_ring *ring);
/* Producer: as many samples as fit. */
size_t mv_ring_write(mv_ring *ring, const float *samples, size_t count);
/* Consumer: up to count samples, after dropping what a pending flush asked to. */
size_t mv_ring_read(mv_ring *ring, float *out, size_t count);
/* Producer: drop everything queued so far. Samples written afterwards still play. The consumer carries it
 * out on its next read, but mv_ring_available counts it at once, so a consumer that has stopped reading
 * (a stalled speaker) cannot keep the queue looking full. */
void mv_ring_request_flush(mv_ring *ring);
/* Samples queued and not flushed. */
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

#include <stdbool.h>

/* When the speaker starts and stops taking from the ring. A reply streams in from Mistral no faster than — and
 * sometimes slower than — it plays, so a ring read straight into every PipeWire quantum ran dry again and again
 * mid-sentence and each gap was a slice of silence cut into the voice: static. The player waits until
 * MV_PLAYER_PREBUFFER_MS is queued (or the writer has gone quiet: the reply's tail), fades in, and when the ring
 * does run dry mid-reply fades out and waits for the buffer again, so a slow network is a pause, not crackle.
 * Samples are clamped to [-1, 1]. PipeWire's thread owns it. */
#define MV_PLAYER_PREBUFFER_MS 250
#define MV_PLAYER_WRITER_IDLE_MS 150
#define MV_PLAYER_FADE 120                /* samples: 5 ms at 24 kHz */

typedef struct mv_player {
    size_t prebuffer;                     /* samples to have queued before playing */
    bool primed;
    size_t fade_in;                       /* samples of the fade-in still to apply */
    size_t underruns, quanta;             /* this reply's, so far */
    size_t last_underruns, last_quanta;   /* the reply that last ended */
} mv_player;

void mv_player_init(mv_player *p, size_t prebuffer);
/* Fills `out` with `frames` samples: what the ring can give once primed, silence for the rest. `writer_idle` is
 * whether the producer has written nothing for MV_PLAYER_WRITER_IDLE_MS. Returns the samples taken from the ring;
 * *ended (may be NULL) is set when a reply finished playing — the ring empty with the writer quiet. */
size_t mv_player_fill(mv_player *p, mv_ring *r, float *out, size_t frames, bool writer_idle, bool *ended);

#endif
