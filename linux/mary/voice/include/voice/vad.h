/* MaryVoice/VAD/EnergyVAD.swift, with VoicePipelineConfig.swift's VADConfig: energy
 * endpointing. An utterance opens when a frame's RMS reaches the start threshold,
 * stays open above the lower continue threshold (hysteresis keeps soft syllables),
 * and closes after the hangover of trailing silence — plus EndpointHold's extension
 * while the phrase looks unfinished. A burst shorter than min_utterance is noise.
 * threshold_boost multiplies both thresholds (the barge-in guard while Mary speaks). */
#ifndef MARY_VOICE_VAD_H
#define MARY_VOICE_VAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct mv_vad_config {
    float speech_start_rms;         /* 0.015 */
    float speech_continue_rms;      /* 0.008 */
    int hangover_ms;                /* 850 */
    int min_utterance_ms;           /* 300 */
    int pre_roll_ms;                /* 600: audio from before the start replayed into transcription */
    float barge_in_rms_boost;       /* 3.0 */
    int barge_resume_ms;            /* 500 */
} mv_vad_config;

mv_vad_config mv_vad_config_default(void);

typedef enum mv_vad_verdict_kind {
    MV_VAD_NONE,
    MV_VAD_SPEECH_START,
    MV_VAD_SPEECH_END,          /* duration: voiced audio, the hangover excluded */
    MV_VAD_DISCARDED_NOISE,
} mv_vad_verdict_kind;

typedef struct mv_vad_verdict {
    mv_vad_verdict_kind kind;
    double duration;
} mv_vad_verdict;

typedef struct mv_vad {
    mv_vad_config config;
    float threshold_boost;          /* 1.0 */
    double hangover_extension;      /* seconds; cleared whenever an utterance opens or closes */
    bool speech_active;
    double voiced;
    double silence;
} mv_vad;

void mv_vad_init(mv_vad *vad, const mv_vad_config *config);
mv_vad_verdict mv_vad_process(mv_vad *vad, float rms, double frame_duration);
void mv_vad_reset(mv_vad *vad);

/* A frame's RMS on the 0…1 scale Swift's vDSP_rmsqv gives float samples. */
float mv_rms_s16(const int16_t *samples, size_t count);
float mv_rms_f32(const float *samples, size_t count);

#endif
