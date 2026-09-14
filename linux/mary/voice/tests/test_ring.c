#include <errno.h>
#include <pthread.h>

#include "mary_test.h"
#include "voice/audio.h"
#include "voice/ring.h"

MARY_TEST(the_ring_keeps_order_and_refuses_what_does_not_fit) {
    mv_ring r;
    MARY_ASSERT_EQ(mv_ring_init(&r, 6), 0);
    MARY_ASSERT_EQ(mv_ring_capacity(&r), 8);
    const float in[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    MARY_ASSERT_EQ(mv_ring_write(&r, in, 10), 8);
    float out[10];
    MARY_ASSERT_EQ(mv_ring_read(&r, out, 3), 3);
    MARY_ASSERT_NEAR(out[2], 2, 0);
    MARY_ASSERT_EQ(mv_ring_write(&r, in + 8, 2), 2);    /* wraps */
    MARY_ASSERT_EQ(mv_ring_available(&r), 7);
    MARY_ASSERT_EQ(mv_ring_read(&r, out, 10), 7);
    MARY_ASSERT_NEAR(out[0], 3, 0);
    MARY_ASSERT_NEAR(out[6], 9, 0);
    mv_ring_free(&r);
}

MARY_TEST(a_flush_empties_the_ring_at_the_next_read) {
    mv_ring r;
    mv_ring_init(&r, 16);
    const float in[4] = { 1, 1, 1, 1 };
    mv_ring_write(&r, in, 4);
    mv_ring_request_flush(&r);
    float out[4];
    MARY_ASSERT_EQ(mv_ring_read(&r, out, 4), 0);
    MARY_ASSERT_EQ(mv_ring_available(&r), 0);
    mv_ring_write(&r, in, 2);
    MARY_ASSERT_EQ(mv_ring_read(&r, out, 4), 2);   /* new audio after the flush plays */
    mv_ring_free(&r);
}

MARY_TEST(a_flush_drops_only_what_was_queued_before_it) {
    mv_ring r;
    mv_ring_init(&r, 16);
    const float old[4] = { 1, 1, 1, 1 }, fresh[2] = { 2, 2 };
    mv_ring_write(&r, old, 4);
    mv_ring_request_flush(&r);
    mv_ring_write(&r, fresh, 2);                   /* the next reply, before the speaker reads again */
    float out[8];
    MARY_ASSERT_EQ(mv_ring_read(&r, out, 8), 2);
    MARY_ASSERT_NEAR(out[0], 2, 0);
    MARY_ASSERT_NEAR(out[1], 2, 0);
    mv_ring_free(&r);
}

MARY_TEST(a_flush_counts_at_once_even_if_the_reader_never_runs) {
    mv_ring r;
    mv_ring_init(&r, 16);
    const float in[5] = { 1, 1, 1, 1, 1 };
    mv_ring_write(&r, in, 5);
    mv_ring_request_flush(&r);
    MARY_ASSERT_EQ(mv_ring_available(&r), 0);     /* a stalled speaker: nothing left to wait for */
    mv_ring_write(&r, in, 3);
    MARY_ASSERT_EQ(mv_ring_available(&r), 3);
    mv_ring_request_flush(&r);
    mv_ring_request_flush(&r);                     /* twice before a read: still just a drop */
    MARY_ASSERT_EQ(mv_ring_available(&r), 0);
    float out[4];
    MARY_ASSERT_EQ(mv_ring_read(&r, out, 4), 0);
    MARY_ASSERT_EQ(mv_ring_available(&r), 0);
    mv_ring_free(&r);
}

static mv_ring shared;
static float got_sum;

static void *consume(void *arg) {
    float out[64];
    size_t total = 0;
    while (total < 100000) {
        size_t n = mv_ring_read(&shared, out, 64);
        for (size_t i = 0; i < n; i++) got_sum += out[i];
        total += n;
    }
    return NULL;
}

MARY_TEST(one_writer_and_one_reader_share_it_without_locks) {
    mv_ring_init(&shared, 1024);
    pthread_t reader;
    got_sum = 0;
    pthread_create(&reader, NULL, consume, NULL);
    float one[100];
    for (int i = 0; i < 100; i++) one[i] = 1;
    size_t written = 0;
    while (written < 100000) {
        size_t want = 100000 - written < 100 ? 100000 - written : 100;
        written += mv_ring_write(&shared, one, want);
    }
    pthread_join(reader, NULL);
    MARY_ASSERT_NEAR(got_sum, 100000, 0.5);
    mv_ring_free(&shared);
}

struct frames {
    int count;
    int16_t first_of_second;
};

static void on_frame(const int16_t *frame, size_t count, void *user) {
    struct frames *f = user;
    if (count != 320) MARY_FAIL("a frame of %zu samples", count);
    if (++f->count == 2) f->first_of_second = frame[0];
}

MARY_TEST(the_framer_cuts_whatever_arrives_into_20_ms_frames) {
    mv_framer framer;
    MARY_ASSERT_EQ(mv_framer_init(&framer, 320), 0);
    int16_t chunk[256];
    int16_t next = 0;
    struct frames f = { 0 };
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 256; j++) chunk[j] = next++;
        mv_framer_push(&framer, chunk, 256, on_frame, &f);
    }
    MARY_ASSERT_EQ(f.count, 3);                /* 1024 samples: three frames and 64 left */
    MARY_ASSERT_EQ(f.first_of_second, 320);
    MARY_ASSERT_EQ(framer.filled, 64);
    mv_framer_reset(&framer);
    MARY_ASSERT_EQ(framer.filled, 0);
    mv_framer_free(&framer);
}

MARY_TEST(audio_defaults_are_what_voxtral_wants) {
    mv_audio_config c = mv_audio_config_default();
    MARY_ASSERT_EQ(c.capture_rate, 16000);
    MARY_ASSERT_EQ(c.capture_frame, 320);
    MARY_ASSERT_EQ(c.playback_rate, 24000);
#ifndef HAVE_PIPEWIRE
    int error = 0;
    MARY_ASSERT(mv_audio_open(&c, NULL, NULL, &error) == NULL);
    MARY_ASSERT_EQ(error, -ENOSYS);
#endif
}

static void queue(mv_ring *r, size_t n, float v) {
    float chunk[64];
    for (size_t i = 0; i < 64; i++) chunk[i] = v;
    while (n) {
        size_t k = n < 64 ? n : 64;
        MARY_ASSERT_EQ(mv_ring_write(r, chunk, k), k);
        n -= k;
    }
}

MARY_TEST(the_player_waits_for_a_buffer_then_fades_in) {
    mv_ring r;
    MARY_ASSERT_EQ(mv_ring_init(&r, 4096), 0);
    mv_player p;
    mv_player_init(&p, 480);
    float out[64];
    queue(&r, 100, 0.5f);
    MARY_ASSERT_EQ(mv_player_fill(&p, &r, out, 64, false, NULL), 0);          /* a trickle: silence, not a slice */
    MARY_ASSERT(out[0] == 0 && out[63] == 0);
    queue(&r, 400, 0.5f);
    MARY_ASSERT_EQ(mv_player_fill(&p, &r, out, 64, false, NULL), 64);         /* the buffer is there: it plays */
    MARY_ASSERT(out[0] < 0.01f);                                              /* from nothing … */
    MARY_ASSERT(out[63] > out[0] && out[63] < 0.5f);
    float prev = out[63];
    MARY_ASSERT_EQ(mv_player_fill(&p, &r, out, 64, false, NULL), 64);
    MARY_ASSERT(out[0] >= prev);                                              /* … rising across quanta, no step */
    MARY_ASSERT(out[63] > 0.49f);                                             /* full once MV_PLAYER_FADE has passed */
    mv_ring_free(&r);
}

MARY_TEST(running_dry_mid_reply_fades_out_and_waits_while_the_tail_simply_plays) {
    mv_ring r;
    MARY_ASSERT_EQ(mv_ring_init(&r, 4096), 0);
    mv_player p;
    mv_player_init(&p, 480);
    float out[256];
    queue(&r, 600, 0.5f);
    MARY_ASSERT_EQ(mv_player_fill(&p, &r, out, 256, false, NULL), 256);
    MARY_ASSERT_EQ(mv_player_fill(&p, &r, out, 256, false, NULL), 256);
    bool ended = true;
    MARY_ASSERT_EQ(mv_player_fill(&p, &r, out, 256, false, &ended), 88);    /* the network fell behind */
    MARY_ASSERT(!ended);
    MARY_ASSERT(out[87] < 0.01f && out[0] > 0.3f);                           /* faded to the silence after it */
    MARY_ASSERT_EQ(p.underruns, 1);
    queue(&r, 100, 0.5f);
    MARY_ASSERT_EQ(mv_player_fill(&p, &r, out, 256, false, NULL), 0);         /* waits for the buffer again */
    /* the reply's tail: fewer than the buffer, and maryd has stopped writing — it plays, and the reply ends */
    MARY_ASSERT_EQ(mv_player_fill(&p, &r, out, 256, true, &ended), 100);
    MARY_ASSERT(ended);
    MARY_ASSERT_EQ(p.last_underruns, 1);
    MARY_ASSERT_EQ(p.underruns, 0);
    MARY_ASSERT_EQ(mv_player_fill(&p, &r, out, 256, true, &ended), 0);
    MARY_ASSERT(!ended && out[0] == 0);
    mv_ring_free(&r);
}

MARY_TEST(the_player_clamps_what_would_clip) {
    mv_ring r;
    MARY_ASSERT_EQ(mv_ring_init(&r, 4096), 0);
    mv_player p;
    mv_player_init(&p, 1);
    queue(&r, 400, 3.0f);
    float out[400];
    mv_player_fill(&p, &r, out, 400, false, NULL);
    for (int i = 0; i < 400; i++) MARY_ASSERT(out[i] <= 1.0f && out[i] >= -1.0f);
    MARY_ASSERT(out[399] == 1.0f);
    mv_ring_free(&r);
}

int main(void) {
    MARY_RUN(the_ring_keeps_order_and_refuses_what_does_not_fit);
    MARY_RUN(a_flush_empties_the_ring_at_the_next_read);
    MARY_RUN(a_flush_drops_only_what_was_queued_before_it);
    MARY_RUN(a_flush_counts_at_once_even_if_the_reader_never_runs);
    MARY_RUN(one_writer_and_one_reader_share_it_without_locks);
    MARY_RUN(the_framer_cuts_whatever_arrives_into_20_ms_frames);
    MARY_RUN(audio_defaults_are_what_voxtral_wants);
    MARY_RUN(the_player_waits_for_a_buffer_then_fades_in);
    MARY_RUN(running_dry_mid_reply_fades_out_and_waits_while_the_tail_simply_plays);
    MARY_RUN(the_player_clamps_what_would_clip);
    MARY_TEST_MAIN_END();
}
