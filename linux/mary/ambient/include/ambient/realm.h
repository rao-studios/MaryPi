/* MaryAmbient/Ambient/Realm/AmbientRealm.swift, Engine/AmbientRealmResolver.swift and
 * Reference/FocusSignal.swift (+ the tracker's ledger) in C: who could serve this turn, and
 * which of them did. Need → candidates (sorted by place token, never by score) → place
 * (named > conforming focus lead > strongest conforming co-active > none). */
#ifndef MARY_AMBIENT_REALM_H
#define MARY_AMBIENT_REALM_H

#include <stdbool.h>

#include "ambient/intent.h"
#include "ambient/place.h"

/* ---- focus: the responder layer behind the lead ---- */

typedef enum ma_evidence_kind { MA_EVIDENCE_GLANCE = 0, MA_EVIDENCE_ACTIVATION = 1, MA_EVIDENCE_ACTIVITY = 2 } ma_evidence_kind;
const char *ma_evidence_name(ma_evidence_kind k);   /* glance, activation, activity */

typedef struct ma_focus_evidence {
    ma_place place;
    ma_evidence_kind kind;
    double at;
} ma_focus_evidence;

#define MA_CO_ACTIVE_MAX 16
#define MA_CO_ACTIVE_HORIZON 300.0      /* activity and activation keep a place co-active this long */
#define MA_GLANCE_HORIZON 600.0
#define MA_SIGNAL_HORIZON 1200.0        /* the lead's own horizon */

typedef struct ma_focus_signal {
    bool has_lead;
    ma_place lead;
    ma_place co_active[MA_CO_ACTIVE_MAX];   /* fresh non-lead places, strongest evidence then recency */
    int co_active_count;
    ma_place glanced[MA_CO_ACTIVE_MAX];     /* the subset whose freshest evidence is a glance */
    int glanced_count;
} ma_focus_signal;

#define MA_FOCUS_LEDGER_MAX 32

/* One stamp per place: the freshest evidence. Activation and activity also take the lead. */
typedef struct ma_focus_ledger {
    ma_focus_evidence entries[MA_FOCUS_LEDGER_MAX];
    int count;
    bool has_lead;
    ma_place lead;
    double lead_at;
} ma_focus_ledger;

void ma_focus_init(ma_focus_ledger *l);
void ma_focus_note(ma_focus_ledger *l, const ma_place *place, ma_evidence_kind kind, double now);
void ma_focus_project(const ma_focus_ledger *l, double now, ma_focus_signal *out);
/* Evidence still inside its horizon. Returns how many. */
int ma_focus_fresh_evidence(const ma_focus_ledger *l, double now, ma_focus_evidence *out, int max);
double ma_focus_horizon(ma_evidence_kind kind);

/* ---- the realm ---- */

#define MA_NEED_ABILITIES 4
#define MA_CANDIDATES_MAX 16
#define MA_NAMED_MAX 8

typedef struct ma_need {
    char abilities[MA_NEED_ABILITIES][32];  /* sorted by id */
    int ability_count;
    char discipline[32];                    /* "" when no cue named one */
} ma_need;

bool ma_need_is_empty(const ma_need *n);

typedef struct ma_candidate {
    ma_place place;
    char conforms_by_abilities[MA_NEED_ABILITIES][32];  /* the intersection with the need */
    int conforms_count;
    bool conforms_by_discipline;
    char target_classes[MA_TARGET_CLASSES_MAX][32];
    int target_class_count;
    bool has_eyes;
    bool has_evidence;
    ma_evidence_kind evidence;
    double evidence_age;
} ma_candidate;

bool ma_candidate_conforms(const ma_candidate *c);

typedef struct ma_realm {
    ma_need need;
    ma_candidate candidates[MA_CANDIDATES_MAX];
    int candidate_count;
    bool has_place;
    ma_place place;
    ma_signal decided_by;           /* MA_SIGNAL_NONE while undecided */
    /* The application an action turn would have to open: the place, when it was only named and is not in
     * front (no evidence of it); else, when no place serves, the closest application of the need — the
     * candidate that conforms by the abilities asked for, else by the discipline, ties by token. MaryOS: a
     * skill's call opens its application; the Mac had no such thing. */
    bool has_spawn;
    ma_place spawn;
} ma_realm;

typedef struct ma_realm_inputs {
    const char *utterance;
    const char *ability_query;      /* NULL: the utterance */
    const ma_place *named;          /* the places the user named: an address, not evidence */
    int named_count;
    const char *discipline;         /* the discipline a cue read the turn as, or NULL */
    ma_signal decided_by;
    const ma_focus_signal *focus;   /* NULL: none */
    const ma_focus_evidence *evidence;
    int evidence_count;
    const ma_roster *roster;
    double now;
} ma_realm_inputs;

void ma_realm_need(const ma_realm_inputs *in, ma_need *out);
int ma_realm_candidates(const ma_need *need, const ma_realm_inputs *in, ma_candidate *out, int max);
bool ma_realm_place(const ma_candidate *candidates, int n, const ma_need *need, const ma_realm_inputs *in, ma_place *out);
void ma_realm_resolve(const ma_realm_inputs *in, ma_realm *out);
/* The spawn candidate for a need nobody open serves: the conforming candidate ranked by abilities, then discipline. */
bool ma_realm_spawn(const ma_candidate *candidates, int n, const ma_need *need, ma_place *out);
/* The candidate the realm settled on, when it is in the set. */
const ma_candidate *ma_realm_chosen(const ma_realm *r);

#endif
