/* What maryd sends sewnd to run a turn: SewnRealtimeWire.TurnStart wrapping
 * SewnWire.ChatRequest (MaryBrain/Sewn), with Mary's defaults — max_tokens 1200,
 * temperature 0.4, top_p 0.9, stop ["###", "END"], repetition_penalty 1.1 over 20,
 * client "mary", provider "mistral" — her persona, and the fr_marie_neutral voice. */
#ifndef MARY_BRAIN_REQUEST_H
#define MARY_BRAIN_REQUEST_H

#include "brain/history.h"

#define MB_PERSONA_NAME "Mary"
#define MB_VOICE_ID "fr_marie_neutral"

/* SewnWire.Persona.mary, reworded for MaryOS (PORTING.md deviation 8). */
extern const char MB_PERSONA_VOICE[];

typedef struct mb_turn_request {
    const char *instructions;   /* mb_sewn_instructions */
    const char *owner_id;       /* sewnd takes the owner from credentials; sent to match Mary's wire */
    const char *request_id;
    const char *voice_id;       /* NULL: MB_VOICE_ID */
} mb_turn_request;

/* {"type": "turn.start", "request": {...}, "tts": {"voice_id": ...}}. A new object. */
struct json_object *mb_turn_start(const mb_history *history, const mb_turn_request *request);

#endif
