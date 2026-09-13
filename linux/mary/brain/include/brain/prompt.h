/* The `instructions` a voice turn sends: MaryPrompts.sewnInstructions rendered
 * through PromptPlan.voice (MaryBrain/Prompt), for what a MaryOS turn holds —
 *
 *   sewnPreamble          the clock and the spoken register           rendered
 *   sewnCompany           company first; facts are scenery            rendered
 *   sewnHeading           hands running in parallel                   off: no Lane B on MaryOS yet
 *   persona (exclusive)   read / grounded / converse / insight / in-turn
 *                                                                     converse on a conversational turn, in-turn otherwise
 *   sewnCapability        what her hands do                           the lead place's line, when one leads
 *   sewnRetrieval         memory is the past                          rendered without its reach and sight splices
 *   sewnSightPending, sewnRunningActions                              off: nothing underway
 *   sewnLiveWork          on screen, then held, one authority block   the ambient section (ambient/prompt.h), last
 *
 * The reach and sight splices tell the model Mary's hands reach every app and can
 * look at the screen; on MaryOS the conversation cannot call skills yet, so they
 * would make her promise what she cannot do (PORTING.md deviation 8). */
#ifndef MARY_BRAIN_PROMPT_H
#define MARY_BRAIN_PROMPT_H

#include <stdbool.h>

#include "brain/clock.h"

struct sk_registry;

typedef struct mb_prompt_inputs {
    bool conversational;        /* the route's intent is converse: the converse persona, else in-turn */
    const char *capability;     /* the capability line for the place that leads, or NULL */
    const char *live_work;      /* the rendered ambient section (ma_prompt_live_work), or NULL */
} mb_prompt_inputs;

/* The skills lane's system prompt (PromptPlan.full: identity, clock, spokenRegister, registerSwitch,
 * commandKinds, stepwise, confirmation, the roster, eyesDoctrine, compositionParadigm, the lead's
 * live context, the co-active lines, the held facts), worded for MaryOS. */
typedef struct mb_system_inputs {
    const struct sk_registry *registry;     /* the roster: one line per app, its skills */
    const char *lead_place_name;            /* "TextEdit", or NULL */
    const char *const *lead_context;        /* the lead place's live blocks (surface lines, blocks) */
    int lead_context_count;
    const char *const *co_active;           /* the merged-worlds lines */
    int co_active_count;
    const char *const *held_facts;
    int held_fact_count;
    const char *const *held_mentions;
    int held_mention_count;
} mb_system_inputs;

char *mb_system_prompt(const mb_clock *clock, const mb_system_inputs *inputs);
/* MaryPrompts.orchestratorAddendum and continuationNudge, worded for MaryOS. */
const char *mb_orchestrator_addendum(void);
const char *mb_continuation_nudge(void);

/* Heap; the caller frees. NULL when out of memory. */
char *mb_sewn_instructions(const mb_clock *clock);
/* The same, with what the route and the ambient store supply. */
char *mb_sewn_instructions_with(const mb_clock *clock, const mb_prompt_inputs *inputs);

#endif
