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

typedef struct mb_prompt_inputs {
    bool conversational;        /* the route's intent is converse: the converse persona, else in-turn */
    const char *capability;     /* the capability line for the place that leads, or NULL */
    const char *live_work;      /* the rendered ambient section (ma_prompt_live_work), or NULL */
} mb_prompt_inputs;

/* Heap; the caller frees. NULL when out of memory. */
char *mb_sewn_instructions(const mb_clock *clock);
/* The same, with what the route and the ambient store supply. */
char *mb_sewn_instructions_with(const mb_clock *clock, const mb_prompt_inputs *inputs);

#endif
