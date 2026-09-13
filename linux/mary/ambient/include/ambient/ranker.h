/* MaryAmbient/Ambient/Ranker/AmbientRanker*.swift and Models/AmbientRankingMode.swift in C:
 * the held facts ranked against the utterance under the user's three-way rule, rendered
 * under a budget — three blocks in full at most, the rest as one-line mentions, never
 * silence. The live surface lines are tier 0, rendered and charged first. */
#ifndef MARY_AMBIENT_RANKER_H
#define MARY_AMBIENT_RANKER_H

#include <stdbool.h>

#include "ambient/render.h"
#include "ambient/store.h"

#define MA_VOICE_BUDGET 1400
#define MA_ABILITY_BUDGET 700
#define MA_MAX_BLOCKS 3
#define MA_CO_ACTIVE_PLACE_BUDGET 280
#define MA_CO_ACTIVE_TOTAL_BUDGET 900
#define MA_RENDER_SURFACES 8
#define MA_RENDER_MENTIONS 32
#define MA_RENDER_KEYS 48
#define MA_KEY_MAX 512

typedef enum ma_ranking_mode { MA_RANKING_RELEVANCE, MA_RANKING_FOCUSED_WORLD, MA_RANKING_TRANSFORM_UNFOCUSED } ma_ranking_mode;
const char *ma_ranking_mode_name(ma_ranking_mode m);    /* relevance, focusedWorld, transformUnfocused */

typedef struct ma_rendering {
    ma_ranking_mode mode;
    char surface_lines[MA_RENDER_SURFACES][MA_SURFACE_LINE_MAX];
    int surface_count;
    char blocks[MA_MAX_BLOCKS][MA_BLOCK_MAX];
    int block_count;
    char mentions[MA_RENDER_MENTIONS][MA_MENTION_MAX];
    int mention_count;
    char keys[MA_RENDER_KEYS][MA_KEY_MAX];      /* behind blocks + mentions, in that order */
    int key_count;
} ma_rendering;

bool ma_rendering_is_empty(const ma_rendering *r);

/* Words worth matching on: lowercase, longer than two letters, not a stop word. Returns how many. */
int ma_tokens(const char *text, char out[][32], int max);
bool ma_is_stop_word(const char *word);

bool ma_concerns_eyeless(const ma_fact *f, const char *utterance, const ma_roster *r);
double ma_relevance(const ma_fact *f, const char *utterance, double now);

/* The three-way rule over places. `focused` may be NULL. */
ma_ranking_mode ma_ranker_mode(const char *utterance, const ma_place *focused, const ma_roster *r);
bool ma_concerns_focused_place(const char *utterance, const ma_place *focused, const ma_roster *r);
/* The discipline a cue names, or NULL. */
const char *ma_named_discipline(const char *utterance, const ma_roster *r);
/* Which places an utterance names: the discipline cue's realm (with eyes) and every registration mentioned. */
int ma_named_places(const char *utterance, const ma_roster *r, ma_place *out, int max);
/* The places the user actually named. */
int ma_explicitly_named_places(const char *utterance, const ma_roster *r, ma_place *out, int max);
bool ma_is_deictic(const char *utterance);
bool ma_references_selection(const char *utterance);
bool ma_references_application_anaphorically(const char *utterance);
bool ma_names_transform(const char *utterance, const ma_roster *r);

/* Sorts `facts` in place under the rule (a fresh direct-reference world puts its fact first). */
ma_ranking_mode ma_rank(ma_fact *facts, int n, const char *utterance, const ma_place *focused, const ma_world *world, double now, const ma_roster *r);

typedef struct ma_render_inputs {
    const ma_fact *facts;
    int fact_count;
    const char *utterance;
    const ma_place *focused;                /* NULL: none */
    const ma_world *world;                  /* NULL: none */
    const char *const *already_rendered;    /* fact keys already in front of the model in full */
    int already_count;
    const char *const *suppressed;          /* passages rendered on their own authority */
    int suppressed_count;
    const ma_surface *const *surfaces;      /* tier 0, in the caller's order (lead first) */
    int surface_count;
    int budget;                             /* 0: MA_VOICE_BUDGET */
    int max_blocks;                         /* 0: MA_MAX_BLOCKS */
    double now;
    const ma_roster *roster;
} ma_render_inputs;

/* Rank, then render. */
void ma_render(const ma_render_inputs *in, ma_rendering *out);

/* The merged-worlds section: compact lines for the places with fresh evidence beside the lead.
 * Returns how many lines; the keys of the facts carried ride in `keys`. */
int ma_co_active_lines(const ma_place *places, const bool *glanced, int place_count, const ma_fact *facts, int fact_count,
                       int place_budget, int total_budget, double now, const ma_roster *r,
                       char lines[][MA_MENTION_MAX * 2], int max_lines, char keys[][MA_KEY_MAX], int max_keys, int *key_count);

#endif
