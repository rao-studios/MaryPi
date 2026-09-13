#include "voice/state.h"

#include <string.h>

static const char *const NAMES[MV_STATE_COUNT] = { "idle", "listening", "hearing", "transcribing", "thinking", "speaking", "error" };

const char *mv_state_name(mv_state state) { return (unsigned)state < MV_STATE_COUNT ? NAMES[state] : NULL; }

bool mv_state_from_name(const char *name, mv_state *out) {
    for (int i = 0; name && i < MV_STATE_COUNT; i++) {
        if (strcmp(name, NAMES[i]) == 0) {
            if (out) *out = (mv_state)i;
            return true;
        }
    }
    return false;
}
