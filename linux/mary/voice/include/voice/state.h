/* MaryVoice/VoicePipelineEvent.swift's VoicePipelineState, named the way maryd
 * tells the desktop. listening(utteranceActive: true) is "hearing" — the session
 * window's "hearing you". "error" is maryd's own: the loop stopped on a failure. */
#ifndef MARY_VOICE_STATE_H
#define MARY_VOICE_STATE_H

#include <stdbool.h>

typedef enum mv_state {
    MV_STATE_IDLE,
    MV_STATE_LISTENING,         /* the mic is open, nothing heard yet */
    MV_STATE_HEARING,           /* an utterance is open */
    MV_STATE_TRANSCRIBING,      /* the utterance closed; waiting on the final text */
    MV_STATE_THINKING,          /* the turn is running; no audio yet */
    MV_STATE_SPEAKING,          /* the reply is playing */
    MV_STATE_ERROR,
    MV_STATE_COUNT
} mv_state;

const char *mv_state_name(mv_state state);
bool mv_state_from_name(const char *name, mv_state *out);

#endif
