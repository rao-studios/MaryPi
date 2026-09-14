/* MaryBrain/Prompt/PromptCatalog+Voice.swift's sewnLiveWork and sewnCapability, and
 * LiveWorkWorld.swift, in C: the ambient section of the voice's instructions — what is on
 * screen, then what she still holds, one authority block that lands last — and the
 * capability line for the place that leads. */
#ifndef MARY_AMBIENT_PROMPT_H
#define MARY_AMBIENT_PROMPT_H

#include <stdbool.h>
#include <stddef.h>

#include "ambient/place.h"
#include "ambient/ranker.h"
#include "ambient/store.h"

typedef enum ma_live_work_kind { MA_LIVE_WORK_UNLED, MA_LIVE_WORK_APPLICATION, MA_LIVE_WORK_DOCUMENT } ma_live_work_kind;

/* Where the live work came from — stated by the arbiter, never inferred. */
typedef struct ma_live_work_world {
    ma_live_work_kind kind;
    char name[MA_NAME_MAX];     /* the application's display name, or "" */
    bool whole;                 /* a document: the whole of it is in hand */
} ma_live_work_world;

/* LiveWorkWorld(machine:): from the turn's machine state. */
void ma_live_work_from_world(const ma_world *world, const ma_roster *r, ma_live_work_world *out);
/* The lead surface names the place: a document when it shows one, else the application. */
void ma_live_work_from_surface(const ma_surface *surface, const ma_roster *r, ma_live_work_world *out);

struct mc_buf;
/* sewnLiveWork: "" when nothing is in hand, else "\n\nYou can see what the user is looking at right now — …".
 * The surface lines are the live work; the blocks are the held facts; the mentions ride below them. */
int ma_prompt_live_work(const ma_rendering *rendering, const ma_live_work_world *world, bool inspired_sight, struct mc_buf *out);
/* MaryPrompts.capabilityLine: what her hands do in the place that leads, or "" when nothing leads. */
void ma_capability_line(const ma_place *lead, const ma_roster *r, char *out, size_t n);
/* The line for an application the turn would open (the realm's spawn): it is not open, and a call to one of
 * its skills opens it with a fresh document. */
void ma_spawn_line(const ma_place *spawn, const ma_roster *r, char *out, size_t n);

#endif
