/* MaryAmbient/Ambient/Engine/AmbientIntent.swift in C: the shape of a turn,
 * and the signal that decided it. Names are the Swift raw values, so a route
 * logged by either side reads the same. */
#ifndef MARY_AMBIENT_INTENT_H
#define MARY_AMBIENT_INTENT_H

#include <stdbool.h>

typedef enum ma_intent {
    MA_INTENT_ARCHITECT,
    MA_INTENT_DECIDE,
    MA_INTENT_HALT,
    MA_INTENT_REVISE,
    MA_INTENT_COMPOSE,
    MA_INTENT_OPERATE,
    MA_INTENT_PERCEIVE,
    MA_INTENT_ASK,
    MA_INTENT_CONVERSE,
    MA_INTENT_COUNT
} ma_intent;

typedef enum ma_signal {
    MA_SIGNAL_ARCHITECT_ABILITY,
    MA_SIGNAL_PENDING_DECISION,
    MA_SIGNAL_ROUTINE_STOP,
    MA_SIGNAL_EDIT_INTENT,
    MA_SIGNAL_EMBEDDING,
    MA_SIGNAL_ACTION_COMMAND,
    MA_SIGNAL_WRITING_REGISTER,
    MA_SIGNAL_WORLD,                    /* raw value "attention" */
    MA_SIGNAL_DEIXIS,
    MA_SIGNAL_NAMED_LEAD_ATTENTION,     /* raw value "namedLeadWorld" */
    MA_SIGNAL_NAMED_PART,
    MA_SIGNAL_AMBIENT_SOURCE,
    MA_SIGNAL_NONE,
    MA_SIGNAL_COUNT
} ma_signal;

const char *ma_intent_name(ma_intent intent);            /* "converse"; NULL out of range */
bool ma_intent_from_name(const char *name, ma_intent *out);
/* isActing: does this shape act on the world, or only talk about it? Reporting only. */
bool ma_intent_is_acting(ma_intent intent);
/* touchesExistingProse: the turns the revision contract applies to. */
bool ma_intent_touches_existing_prose(ma_intent intent);

const char *ma_signal_name(ma_signal signal);            /* NULL out of range */

/* What is in front, as the desktop reports it (declared; nothing fills it yet). */
typedef struct ma_focus {
    char app_id[64];        /* lp_app.id: "calendar" */
    char window_id[64];
    char title[256];
    double observed_at;     /* seconds since the Unix epoch */
} ma_focus;

#endif
