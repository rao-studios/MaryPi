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

int main(void) {
    MARY_RUN(the_ring_keeps_order_and_refuses_what_does_not_fit);
    MARY_RUN(a_flush_empties_the_ring_at_the_next_read);
    MARY_RUN(one_writer_and_one_reader_share_it_without_locks);
    MARY_RUN(the_framer_cuts_whatever_arrives_into_20_ms_frames);
    MARY_RUN(audio_defaults_are_what_voxtral_wants);
    MARY_TEST_MAIN_END();
}
