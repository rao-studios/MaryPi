/* MaryVoice/VAD/BargeInGovernor.swift, declared: interrupting Mary by voice while
 * she speaks. In Swift the first frame over the boosted onset pauses playback, 300 ms
 * of voice commits the interruption and 500 ms of quiet resumes. That needs echo
 * cancellation, which MaryOS does not have yet, so for now the governor never fires
 * and Esc or the Ask Mary button stop her instead (PORTING.md deviation 6). */
#ifndef MARY_VOICE_BARGE_IN_H
#define MARY_VOICE_BARGE_IN_H

#include <stdbool.h>

typedef enum mv_barge {
    MV_BARGE_NONE,
    MV_BARGE_PAUSE,     /* provisional: something loud over the speaker */
    MV_BARGE_COMMIT,    /* the user is talking: stop the reply */
    MV_BARGE_RESUME,    /* it was noise: play on */
} mv_barge;

typedef struct mv_barge_in {
    float onset_rms;        /* speech_start_rms × barge_in_rms_boost while audible */
    int commit_ms;          /* 300 */
    int resume_ms;          /* barge_resume_ms, 500 */
    bool enabled;           /* false until echo cancellation exists */
} mv_barge_in;

void mv_barge_in_init(mv_barge_in *governor, float onset_rms, int resume_ms);
/* MV_BARGE_NONE while the governor is disabled. */
mv_barge mv_barge_in_process(mv_barge_in *governor, float rms, double frame_duration, bool audible);

#endif
