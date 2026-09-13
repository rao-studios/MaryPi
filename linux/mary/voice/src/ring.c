#include "voice/ring.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int mv_ring_init(mv_ring *r, size_t capacity) {
    size_t cap = 1;
    while (cap < capacity) cap <<= 1;
    r->data = calloc(cap, sizeof *r->data);
    if (!r->data) return -ENOMEM;
    r->mask = cap - 1;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    atomic_init(&r->flush, false);
    return 0;
}

void mv_ring_free(mv_ring *r) {
    free(r->data);
    r->data = NULL;
}

size_t mv_ring_capacity(const mv_ring *r) { return r->mask + 1; }

size_t mv_ring_available(const mv_ring *r) {
    return atomic_load(&((mv_ring *)r)->head) - atomic_load(&((mv_ring *)r)->tail);
}

size_t mv_ring_write(mv_ring *r, const float *samples, size_t count) {
    size_t head = atomic_load(&r->head), tail = atomic_load(&r->tail);
    size_t room = mv_ring_capacity(r) - (head - tail), n = count < room ? count : room;
    for (size_t i = 0; i < n; i++) r->data[(head + i) & r->mask] = samples[i];
    atomic_store(&r->head, head + n);
    return n;
}

size_t mv_ring_read(mv_ring *r, float *out, size_t count) {
    size_t head = atomic_load(&r->head), tail = atomic_load(&r->tail);
    if (atomic_exchange(&r->flush, false)) {
        atomic_store(&r->tail, head);
        return 0;
    }
    size_t n = head - tail < count ? head - tail : count;
    for (size_t i = 0; i < n; i++) out[i] = r->data[(tail + i) & r->mask];
    atomic_store(&r->tail, tail + n);
    return n;
}

void mv_ring_request_flush(mv_ring *r) { atomic_store(&r->flush, true); }

int mv_framer_init(mv_framer *f, size_t frame_samples) {
    f->frame = calloc(frame_samples ? frame_samples : 1, sizeof *f->frame);
    if (!f->frame) return -ENOMEM;
    f->size = frame_samples;
    f->filled = 0;
    return 0;
}

void mv_framer_free(mv_framer *f) {
    free(f->frame);
    f->frame = NULL;
}

void mv_framer_push(mv_framer *f, const int16_t *samples, size_t count, mv_frame_fn fn, void *user) {
    while (count) {
        size_t n = f->size - f->filled < count ? f->size - f->filled : count;
        memcpy(f->frame + f->filled, samples, n * sizeof *samples);
        f->filled += n;
        samples += n;
        count -= n;
        if (f->filled == f->size) {
            fn(f->frame, f->size, user);
            f->filled = 0;
        }
    }
}

void mv_framer_reset(mv_framer *f) { f->filled = 0; }
