/* MaryBrain/Brain/TurnTriage.swift, Abilities/Roster/EmbeddingRouting.swift, the semantic
 * skill index, MaryPlugin's SpokenArgumentExtractor and SpokenEnumExtractor, and
 * DeterministicTier.swift in C: the one semantic read of a turn, and the shortcut that
 * dispatches a skill with no model round.
 *
 * At registry load every skill's text (title, summary, phrases, tokens) is embedded once
 * (through sewnd's `embed`, the only network there is); a turn embeds its utterance once
 * and every skill's affinity is the cosine. One skill above the floor with a margin over the
 * runner-up is the unique winner; it dispatches without a model when its argument shape is
 * safe (no required argument, one spoken string, or one enum the sentence names) and the
 * sentence is a single clause. ABSTAINS, NEVER GUESSES: without an index every field is
 * empty and the turn falls to the model. */
#ifndef MARY_BRAIN_TRIAGE_H
#define MARY_BRAIN_TRIAGE_H

#include <stdbool.h>
#include <stddef.h>

#include "skills/registry.h"

#define MB_ROUTING_FLOOR 0.62f
#define MB_ROUTING_MARGIN 0.04f
#define MB_SINGLE_CLAUSE_WORDS 12
/* isActionShaped, without the Mac's intent corpus: the words land near some skill at all. */
#define MB_ACTION_FLOOR 0.50f

struct json_object;

/* The embedding seam: n texts → n × *dim floats on the heap. 0, or -errno with `message`. */
typedef int (*mb_embed_fn)(const char *const *texts, size_t n, float **out, size_t *dim, char *message, size_t cap, void *user);
/* Over sewnd's socket (`user` is the socket path). */
int mb_embed_sewn(const char *const *texts, size_t n, float **out, size_t *dim, char *message, size_t cap, void *user);

typedef struct mb_skill_vector {
    char app[64], skill[64];
    char invocation[128];
    float *vector;
} mb_skill_vector;

typedef struct mb_skill_index {
    mb_skill_vector *items;
    size_t n;
    size_t dim;
} mb_skill_index;

/* The text a skill is embedded as: "<title>. <summary> <phrases>. <tokens>." */
void mb_skill_index_text(const sk_app *app, const sk_skill *skill, char *out, size_t n);
/* Embeds every enabled skill (one call, batched). 0; -errno with `message`. */
int mb_skill_index_build(mb_skill_index *index, const sk_registry *registry, mb_embed_fn embed, void *user, char *message, size_t cap);
void mb_skill_index_free(mb_skill_index *index);

typedef struct mb_affinity {
    const mb_skill_vector *skill;
    float score;
} mb_affinity;

/* Every skill's cosine to the query vector, best first. Returns how many. */
int mb_affinities(const mb_skill_index *index, const float *query, mb_affinity *out, int max);
/* uniqueWinner: exactly one above the floor with the margin over the runner-up, among skills the
 * registry still offers (enabled, not denied). NULL otherwise. */
const mb_skill_vector *mb_unique_winner(const mb_affinity *affinities, int n, const sk_registry *registry, float floor, float margin);

typedef enum mb_confidence_shape { MB_SHAPE_NONE, MB_SHAPE_NO_REQUIRED_ARGUMENTS, MB_SHAPE_SINGLE_STRING, MB_SHAPE_SINGLE_ENUM } mb_confidence_shape;
const char *mb_confidence_shape_name(mb_confidence_shape s);

/* confidenceShape: what the shortcut would have to supply, or NONE for "hand it to the model". */
mb_confidence_shape mb_confidence_shape_of(const sk_skill *skill, const char *utterance);
/* isSingleClause: at most 12 words, no joiners, no mid-sentence punctuation. */
bool mb_is_single_clause(const char *utterance);
/* The shortcut's arguments: the one required string takes the peeled span, an enum the value the
 * sentence names, an optional enum it names too. A new object; `stages` (may be NULL) gets one line
 * per peel that changed the text. */
struct json_object *mb_confidence_arguments(const sk_skill *skill, const sk_app *app, const char *utterance, char *stages, size_t stages_cap);

/* SpokenArgumentExtractor.peeled: the preamble, a leading "open <app> and", one leading trigger
 * phrase or token, a trailing "in/on <app>" — each stage backing off to the last non-empty. */
void mb_spoken_span(const char *utterance, const sk_skill *skill, const sk_app *app, char *out, size_t n, char *stages, size_t stages_cap);
/* SpokenEnumExtractor: the one enum value the sentence names (its own name, or its spoken words),
 * or false. `value` and `spoken_as` are filled. */
bool mb_spoken_enum(const sk_skill *skill, const char *parameter, const char *utterance, char *value, size_t vn, char *spoken_as, size_t sn);

/* TurnTriage's isActionShaped, as MaryOS can know it: the best skill affinity clears MB_ACTION_FLOOR, or the
 * request's first content word (after the preamble — "hey mary", "can you", "please") is a trigger token
 * of some skill. The route treats an action-shaped turn as an action turn (MaryBrain+Turn: actionTurn). */
bool mb_action_shaped(const mb_affinity *affinities, int n, const char *utterance, const sk_registry *registry);
/* A question: after the preamble ("hey mary", "can you", "please"), the first word asks (what, who, where, when,
 * why, how, which, whose, is, are, was, were, do, does, did, have, has, had, am). A question is never
 * action-shaped, and a unique winner behind it does not promote the turn — "what did I write in the note"
 * lands near "Write a new note" and must stay a question for the voice and the Thread. */
bool mb_question_shaped(const char *utterance);
/* DeterministicTier.decision: a bare, exact yes or no. 1, 0, or -1. */
int mb_deterministic_decision(const char *utterance);

#endif
