/* MaryAmbient/Ambient/Engine/AmbientEngine.swift, AmbientRoute.swift and AmbientIntentGate.swift
 * in C: a turn resolved once, before the prompt or any skill is assembled. The route is the
 * typed answer to what this turn is (the intent and the signal that decided it), where it leads
 * (the lead application, the realm), what it needs (a locate, a pre-read, execution) and which
 * durable context to consult (the memory plan: the Mac's two logical threads, `ability` and
 * `personal`, which threadd's four lanes stand behind — application and behavioral, personal and
 * conversation). The classify ladder is fixed: pending decision → routine stop → edit intent →
 * architect → embedding intent → action turn → deixis → named lead → ambient source → named part
 * → converse. */
#ifndef MARY_AMBIENT_ENGINE_H
#define MARY_AMBIENT_ENGINE_H

#include <stdbool.h>

#include "ambient/classify.h"
#include "ambient/intent.h"
#include "ambient/place.h"
#include "ambient/ranker.h"
#include "ambient/realm.h"
#include "ambient/store.h"

/* ---- the gate: which durable context to consult ---- */

typedef enum ma_lane { MA_LANE_ABILITY = 1, MA_LANE_PERSONAL = 2 } ma_lane;    /* ThreadLane, as bits */
const char *ma_lane_name(ma_lane l);                 /* ability, personal */
/* The storage lane (thread/families.h) behind a Mac lane: ability → behavioral; personal → personal. */
int ma_lane_storage_lanes(ma_lane l, const char **out, int max);

#define MA_TARGETS_MAX 8
#define MA_HINTS_MAX 24
#define MA_GATE_APPLICATIONS 8

typedef struct ma_ability_target {
    char ability_id[32];
    ma_paradigm paradigm;
} ma_ability_target;

typedef struct ma_memory_plan {
    unsigned lanes;                             /* MA_LANE_* bits; never empty */
    ma_ability_target targets[MA_TARGETS_MAX];  /* sorted by ability id */
    int target_count;
    ma_lane lane_priority[2];
    int priority_count;
    char relationship_hints[MA_HINTS_MAX][32];  /* sorted, unique */
    int hint_count;
    bool expand_discipline_usage;
} ma_memory_plan;

typedef struct ma_signature {
    unsigned questions;
    char predicate_families[MA_HINTS_MAX][32];
    int family_count;
    ma_lane lane_priority[2];
    int priority_count;
    char semantic_projection[2304];
} ma_signature;

typedef struct ma_gate {
    unsigned questions;                         /* MA_Q_* bits */
    char requested_abilities[MA_NEED_ABILITIES][32];
    int requested_count;
    char applications[MA_GATE_APPLICATIONS][MA_ID_MAX];     /* named by their aliases, sorted */
    int application_count;
    ma_signature signature;
    ma_memory_plan memory;
} ma_gate;

void ma_gate_resolve(const char *utterance, const char *routing_query, const char *lead_application_id, const ma_roster *r, ma_gate *out);
/* Every application the gate matched by name, as places. */
int ma_gate_named_places(const ma_gate *g, const ma_roster *r, ma_place *out, int max);

/* ---- verdicts and the route ---- */

typedef struct ma_verdicts {
    bool action_turn;
    bool has_edit;
    ma_edit_intent edit;
    char named_part[MA_EDIT_TARGET_MAX];        /* "" when none */
    bool names_ambient_source;
    bool is_deictic;
    bool names_transform;
    char focus_override[32];                    /* the discipline a cue named, or "" */
    int bare_decision;                          /* 1, 0, or -1 */
} ma_verdicts;

typedef enum ma_writing_target { MA_WRITING_NONE, MA_WRITING_SELECTION, MA_WRITING_PASSAGE } ma_writing_target;
const char *ma_writing_target_name(ma_writing_target t);    /* selection, passage; NULL for none */

typedef struct ma_route {
    ma_intent intent;
    ma_signal decided_by;
    ma_verdicts verdicts;
    ma_gate gate;
    bool has_world;
    ma_world world;                             /* the freshest behavioral signal, for diagnostics */
    bool selection_defines_turn;
    char lead_application_id[MA_ID_MAX];        /* "" when nothing leads */
    ma_place named_places[MA_NAMED_MAX];        /* what the utterance named, plus the gate's */
    int named_count;
    ma_realm realm;
    unsigned candidate_attentions;              /* bits 1 << ma_attention */
    ma_writing_target writing_target;
    char supporting_context[MA_EDIT_TARGET_MAX];
    bool needs_locate, needs_pre_read, needs_execution;
    ma_ranking_mode ranking_mode;
} ma_route;

bool ma_route_lead_place(const ma_route *route, ma_place *out);
/* The post-route answer: the turn acts rather than converses. */
bool ma_route_is_action_turn(const ma_route *route);
/* The world the route accepted as its referent, or NULL (a rejected packet stays on the route for diagnostics). */
const ma_world *ma_route_routed_world(const ma_route *route);

typedef struct ma_engine_inputs {
    const char *utterance;
    const char *routing_query;          /* NULL: the utterance */
    bool action_turn;
    bool has_embedding_intent;
    ma_intent embedding_intent;
    const ma_edit_intent *edit_intent;  /* NULL: classified here from the utterance */
    bool classify_edit;                 /* when edit_intent is NULL: run the classifier (false: no edit intent) */
    int bare_decision;                  /* -2: classified here; else 1, 0 or -1 */
    bool pending_confirmation;
    int active_routines;
    const ma_world *world;              /* this turn's machine state, or NULL */
    const char *lead_application_id;    /* the focus tracker's lead, or NULL */
    const ma_focus_signal *focus;
    const ma_focus_evidence *evidence;
    int evidence_count;
    const ma_roster *roster;
    double now;
} ma_engine_inputs;

void ma_engine_resolve(const ma_engine_inputs *in, ma_route *out);

/* Which of Mary's own lanes supplied routing evidence — a diagnostic, never an allowlist. */
unsigned ma_engine_candidate_attentions(ma_intent intent, const ma_place *lead, const ma_place *named, int named_count, const ma_roster *r);

#endif
