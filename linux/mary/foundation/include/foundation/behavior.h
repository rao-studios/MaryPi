/* MaryFoundation/Behavior/BehavioralEpisode.swift and BehavioralAction.swift in C, with
 * the codec that writes one episode as one JSON line (sorted keys, ISO-8601 dates in
 * UTC with fractional seconds): one turn — what was asked, what was in front of the
 * person, and the actions Mary composed with their outcomes. `mary.behavior` v1. */
#ifndef MARY_FOUNDATION_BEHAVIOR_H
#define MARY_FOUNDATION_BEHAVIOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MF_BEHAVIOR_SCHEMA "mary.behavior"
#define MF_BEHAVIOR_SCHEMA_VERSION 1
#define MF_BEHAVIOR_ACTIONS_MAX 16
#define MF_BEHAVIOR_TARGETS_MAX 8
#define MF_UUID_LEN 36

typedef enum mf_seal_reason { MF_SEAL_NONE, MF_SEAL_COMPLETED, MF_SEAL_SUPERSEDED, MF_SEAL_CANCELLED, MF_SEAL_APP_QUIT } mf_seal_reason;
typedef enum mf_disposition {
    MF_DISPOSITION_SUCCEEDED, MF_DISPOSITION_FAILED, MF_DISPOSITION_BLOCKED, MF_DISPOSITION_DEFERRED, MF_DISPOSITION_CANCELLED,
    MF_DISPOSITION_REQUESTED_CONFIRMATION, MF_DISPOSITION_UNSETTLED,
} mf_disposition;
typedef enum mf_initiator { MF_INITIATOR_MODEL, MF_INITIATOR_MARY_READ, MF_INITIATOR_MARY_ACT } mf_initiator;

const char *mf_seal_reason_name(mf_seal_reason r);          /* completed, superseded, cancelled, appQuit; NULL for none */
const char *mf_disposition_name(mf_disposition d);
const char *mf_initiator_name(mf_initiator i);
bool mf_disposition_did_run(mf_disposition d);

/* AbilitySkillReference: the frozen identity of the skill an action called. */
typedef struct mf_skill_reference {
    char package_id[64];        /* the app (or discipline) that offers it */
    char ability_id[64];
    char skill_id[64];
    char invocation[128];       /* "textedit__save" */
    char version[16];
    char digest[24];
} mf_skill_reference;

/* BehavioralActionRecord: one performable action, and what came of it. */
typedef struct mf_action_record {
    char id[48];                /* the model's own call id, or the lane's */
    char intention[128];        /* the invocation name as the model saw it */
    char arguments_json[512];   /* canonical: sorted keys */
    mf_skill_reference skill;
    char adapters[2][32];       /* "desktop" on MaryOS */
    int adapter_count;
    mf_disposition disposition;
    char summary[240];
    bool found_nothing;
    bool undoable;
    char container_key[64];
    mf_initiator initiator;
} mf_action_record;

typedef struct mf_ability_target {
    char ability_id[64];
    char paradigm[32];          /* discipline | applicationExpertise | systemControl */
} mf_ability_target;

typedef struct mf_behavior_episode {
    char id[MF_UUID_LEN + 1];   /* the user turn's id */
    double opened_at, sealed_at;
    mf_seal_reason sealed_reason;
    char query[1024];
    char *ambient_json;         /* the AmbientCapture as JSON text (heap, owned), or NULL */
    char prior_episode_id[MF_UUID_LEN + 1];
    mf_action_record actions[MF_BEHAVIOR_ACTIONS_MAX];
    int action_count;
    char engine[32], lane[32], app_version[32];   /* EpisodeProvenance */
    mf_ability_target targets[MF_BEHAVIOR_TARGETS_MAX];
    int target_count;
} mf_behavior_episode;

void mf_behavior_open(mf_behavior_episode *e, const char *id, const char *query, double opened_at, const char *engine, const char *lane, const char *app_version);
void mf_behavior_free(mf_behavior_episode *e);
/* Appends; -ENOSPC when full. */
int mf_behavior_append(mf_behavior_episode *e, const mf_action_record *record);
/* Seals once: the first reason wins. */
void mf_behavior_seal(mf_behavior_episode *e, mf_seal_reason reason, double at);
bool mf_behavior_is_sealed(const mf_behavior_episode *e);
bool mf_behavior_did_act(const mf_behavior_episode *e);
/* Targets are kept unique and sorted by (ability, paradigm). */
void mf_behavior_add_target(mf_behavior_episode *e, const char *ability_id, const char *paradigm);

/* BehavioralCodec: the episode as one JSON line with sorted keys (heap; the caller frees). */
char *mf_behavior_encode(const mf_behavior_episode *e);
/* The inverse, for the Thread app's codec view and the tests. 0, or -EBADMSG. */
int mf_behavior_decode(const char *json, size_t len, mf_behavior_episode *out);

/* ISO-8601 in UTC with milliseconds: 2026-09-13T10:00:00.123Z. */
void mf_iso8601(double seconds, char *out, size_t n);
bool mf_iso8601_parse(const char *text, double *seconds);
/* A version-4 UUID, lowercase. */
void mf_uuid_v4(char out[MF_UUID_LEN + 1]);
/* The canonical form of an arguments object: its keys sorted (recursively), compact. Text that is
 * not a JSON object comes back trimmed as written. Heap. */
char *mf_canonical_json(const char *json);

#endif
