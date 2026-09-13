#include "mary_test.h"
#include "voice/vad.h"

/* EnergyVADTests.swift, case for case: 20 ms frames. */
static const double FRAME = 0.02;

static mv_vad make_vad(void) {
    mv_vad_config config = mv_vad_config_default();
    mv_vad vad;
    mv_vad_init(&vad, &config);
    return vad;
}

static void speak(mv_vad *vad, int frames) {
    for (int i = 0; i < frames; i++) mv_vad_process(vad, 0.05f, FRAME);
}

MARY_TEST(silenceNeverOpens) {
    mv_vad vad = make_vad();
    for (int i = 0; i < 200; i++) MARY_ASSERT_EQ(mv_vad_process(&vad, 0.001f, FRAME).kind, MV_VAD_NONE);
    MARY_ASSERT(!vad.speech_active);
}

MARY_TEST(loudFrameOpensUtterance) {
    mv_vad vad = make_vad();
    MARY_ASSERT_EQ(mv_vad_process(&vad, 0.05f, FRAME).kind, MV_VAD_SPEECH_START);
    MARY_ASSERT(vad.speech_active);
}

MARY_TEST(hysteresisKeepsSoftSyllablesAlive) {
    mv_vad vad = make_vad();
    mv_vad_process(&vad, 0.05f, FRAME);
    MARY_ASSERT_EQ(mv_vad_process(&vad, 0.010f, FRAME).kind, MV_VAD_NONE);
    MARY_ASSERT(vad.speech_active);
}

MARY_TEST(hangoverClosesUtterance) {
    mv_vad vad = make_vad();
    speak(&vad, 26);
    int ends = 0;
    double duration = 0;
    for (int i = 0; i < 50; i++) {
        mv_vad_verdict v = mv_vad_process(&vad, 0.001f, FRAME);
        if (v.kind == MV_VAD_SPEECH_END) {
            ends++;
            duration = v.duration;
        }
    }
    MARY_ASSERT_EQ(ends, 1);
    MARY_ASSERT_NEAR(duration, 0.52, 0.05);
    MARY_ASSERT(!vad.speech_active);
}

MARY_TEST(shortBurstDiscardedAsNoise) {
    mv_vad vad = make_vad();
    mv_vad_process(&vad, 0.05f, FRAME);
    bool saw = false;
    for (int i = 0; i < 60; i++) saw |= mv_vad_process(&vad, 0.001f, FRAME).kind == MV_VAD_DISCARDED_NOISE;
    MARY_ASSERT(saw);
}

MARY_TEST(boostRaisesThreshold) {
    mv_vad vad = make_vad();
    vad.threshold_boost = 3.0f;
    MARY_ASSERT_EQ(mv_vad_process(&vad, 0.02f, FRAME).kind, MV_VAD_NONE);
    MARY_ASSERT_EQ(mv_vad_process(&vad, 0.05f, FRAME).kind, MV_VAD_SPEECH_START);
}

MARY_TEST(extensionHoldsAnUnfinishedPhraseOpen) {
    mv_vad vad = make_vad();
    speak(&vad, 26);
    vad.hangover_extension = 0.7;
    for (int i = 0; i < 60; i++) MARY_ASSERT_EQ(mv_vad_process(&vad, 0.001f, FRAME).kind, MV_VAD_NONE);
    MARY_ASSERT(vad.speech_active);
    bool ended = false;
    for (int i = 0; i < 20; i++) ended |= mv_vad_process(&vad, 0.001f, FRAME).kind == MV_VAD_SPEECH_END;
    MARY_ASSERT(ended);
    MARY_ASSERT(vad.hangover_extension == 0);
}

MARY_TEST(resetClearsTheExtension) {
    mv_vad vad = make_vad();
    vad.hangover_extension = 0.7;
    mv_vad_reset(&vad);
    MARY_ASSERT(vad.hangover_extension == 0);
}

MARY_TEST(aStaleExtensionDoesNotReachTheNextUtterance) {
    mv_vad vad = make_vad();
    vad.hangover_extension = 0.7;
    MARY_ASSERT_EQ(mv_vad_process(&vad, 0.05f, FRAME).kind, MV_VAD_SPEECH_START);
    MARY_ASSERT(vad.hangover_extension == 0);
}

MARY_TEST(rms_matches_the_float_scale) {
    const int16_t half[4] = { 16384, -16384, 16384, -16384 };
    MARY_ASSERT_NEAR(mv_rms_s16(half, 4), 0.5, 1e-6);
    const float quarter[2] = { 0.25f, -0.25f };
    MARY_ASSERT_NEAR(mv_rms_f32(quarter, 2), 0.25, 1e-6);
    MARY_ASSERT_NEAR(mv_rms_s16(NULL, 0), 0, 1e-9);
}

int main(void) {
    MARY_RUN(silenceNeverOpens);
    MARY_RUN(loudFrameOpensUtterance);
    MARY_RUN(hysteresisKeepsSoftSyllablesAlive);
    MARY_RUN(hangoverClosesUtterance);
    MARY_RUN(shortBurstDiscardedAsNoise);
    MARY_RUN(boostRaisesThreshold);
    MARY_RUN(extensionHoldsAnUnfinishedPhraseOpen);
    MARY_RUN(resetClearsTheExtension);
    MARY_RUN(aStaleExtensionDoesNotReachTheNextUtterance);
    MARY_RUN(rms_matches_the_float_scale);
    MARY_TEST_MAIN_END();
}
