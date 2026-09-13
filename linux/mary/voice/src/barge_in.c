#include "voice/barge_in.h"

void mv_barge_in_init(mv_barge_in *g, float onset_rms, int resume_ms) {
    *g = (mv_barge_in){ .onset_rms = onset_rms, .commit_ms = 300, .resume_ms = resume_ms, .enabled = false };
}

mv_barge mv_barge_in_process(mv_barge_in *g, float rms, double frame_duration, bool audible) {
    return MV_BARGE_NONE;
}
