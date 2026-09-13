#include "ambient/intent.h"

#include <string.h>

static const char *const INTENT_NAMES[MA_INTENT_COUNT] = {
    "architect", "decide", "halt", "revise", "compose", "operate", "perceive", "ask", "converse",
};

static const char *const SIGNAL_NAMES[MA_SIGNAL_COUNT] = {
    "architectAbility", "pendingDecision", "routineStop", "editIntent", "embedding", "actionCommand",
    "writingRegister", "attention", "deixis", "namedLeadWorld", "namedPart", "ambientSource", "none",
};

const char *ma_intent_name(ma_intent intent) {
    return (unsigned)intent < MA_INTENT_COUNT ? INTENT_NAMES[intent] : NULL;
}

bool ma_intent_from_name(const char *name, ma_intent *out) {
    if (!name) return false;
    for (int i = 0; i < MA_INTENT_COUNT; i++) {
        if (strcmp(name, INTENT_NAMES[i]) == 0) {
            if (out) *out = (ma_intent)i;
            return true;
        }
    }
    return false;
}

bool ma_intent_is_acting(ma_intent intent) {
    switch (intent) {
    case MA_INTENT_REVISE: case MA_INTENT_COMPOSE: case MA_INTENT_OPERATE: case MA_INTENT_DECIDE: case MA_INTENT_HALT:
        return true;
    default:
        return false;
    }
}

bool ma_intent_touches_existing_prose(ma_intent intent) { return intent == MA_INTENT_REVISE; }

const char *ma_signal_name(ma_signal signal) {
    return (unsigned)signal < MA_SIGNAL_COUNT ? SIGNAL_NAMES[signal] : NULL;
}
