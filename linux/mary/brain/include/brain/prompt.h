/* The `instructions` a voice turn sends: MaryPrompts.sewnInstructions rendered
 * through PromptPlan.voice (MaryBrain/Prompt), for what a MaryOS turn holds —
 *
 *   sewnPreamble          the clock and the spoken register           rendered
 *   sewnCompany           company first; facts are scenery            rendered
 *   sewnHeading           hands running in parallel                   off: no Lane B on MaryOS yet
 *   persona (exclusive)   read / grounded / converse / insight / in-turn
 *                                                                     converse — every MaryOS turn is conversation for now
 *   sewnCapability        what her hands do                           off: no capability line
 *   sewnRetrieval         memory is the past                          rendered without its reach and sight splices
 *   sewnSightPending, sewnRunningActions, sewnLiveWork                off: nothing on screen or underway
 *
 * The reach and sight splices tell the model Mary's hands reach every app and can
 * look at the screen; on MaryOS the conversation cannot call skills yet, so they
 * would make her promise what she cannot do (PORTING.md deviation 8). */
#ifndef MARY_BRAIN_PROMPT_H
#define MARY_BRAIN_PROMPT_H

#include "brain/clock.h"

/* Heap; the caller frees. NULL when out of memory. */
char *mb_sewn_instructions(const mb_clock *clock);

#endif
