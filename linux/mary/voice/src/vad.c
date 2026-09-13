#include "voice/vad.h"

#include <math.h>
#include <string.h>

mv_vad_config mv_vad_config_default(void) {
    return (mv_vad_config){
        .speech_start_rms = 0.015f,
        .speech_continue_rms = 0.008f,
        .hangover_ms = 850,
        .min_utterance_ms = 300,
        .pre_roll_ms = 600,
        .barge_in_rms_boost = 3.0f,
        .barge_resume_ms = 500,
    };
}

void mv_vad_init(mv_vad *vad, const mv_vad_config *config) {
    memset(vad, 0, sizeof *vad);
    vad->config = config ? *config : mv_vad_config_default();
    vad->threshold_boost = 1.0f;
}

mv_vad_verdict mv_vad_process(mv_vad *vad, float rms, double frame_duration) {
    float start = vad->config.speech_start_rms * vad->threshold_boost;
    float keep = vad->config.speech_continue_rms * vad->threshold_boost;
    if (!vad->speech_active) {
        if (rms >= start) {
            vad->speech_active = true;
            vad->voiced = frame_duration;
            vad->silence = 0;
            vad->hangover_extension = 0;   /* a late partial from the last utterance must not hold this one */
            return (mv_vad_verdict){ MV_VAD_SPEECH_START, 0 };
        }
        return (mv_vad_verdict){ MV_VAD_NONE, 0 };
    }
    if (rms >= keep) {
        vad->voiced += frame_duration;
        vad->silence = 0;
        return (mv_vad_verdict){ MV_VAD_NONE, 0 };
    }
    vad->silence += frame_duration;
    if (vad->silence < (double)vad->config.hangover_ms / 1000 + vad->hangover_extension)
        return (mv_vad_verdict){ MV_VAD_NONE, 0 };
    double duration = vad->voiced;
    vad->speech_active = false;
    vad->voiced = 0;
    vad->silence = 0;
    vad->hangover_extension = 0;
    if (duration < (double)vad->config.min_utterance_ms / 1000) return (mv_vad_verdict){ MV_VAD_DISCARDED_NOISE, 0 };
    return (mv_vad_verdict){ MV_VAD_SPEECH_END, duration };
}

void mv_vad_reset(mv_vad *vad) {
    vad->speech_active = false;
    vad->voiced = 0;
    vad->silence = 0;
    vad->hangover_extension = 0;
}

float mv_rms_s16(const int16_t *samples, size_t count) {
    if (!count) return 0;
    double sum = 0;
    for (size_t i = 0; i < count; i++) {
        double x = samples[i] / 32768.0;
        sum += x * x;
    }
    return (float)sqrt(sum / (double)count);
}

float mv_rms_f32(const float *samples, size_t count) {
    if (!count) return 0;
    double sum = 0;
    for (size_t i = 0; i < count; i++) sum += (double)samples[i] * samples[i];
    return (float)sqrt(sum / (double)count);
}
