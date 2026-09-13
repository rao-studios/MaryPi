/* sewnd's turn: Sewn's realtime route (API/Routes/Realtime/Realtime.swift and
 * RealtimeTurnEngine.swift) cut to one grounded pass with no retrieval. The
 * connection's first frame is `turn.start{request, tts{voice_id, model}}`, where
 * `request` is Mary's ChatRequest — the JSON Sewn accepts — and sewnd answers on
 * the same connection:
 *
 *   phase{phase: "grounded"}                                      once
 *   token{phase: "grounded", text}                                as Mistral streams
 *   audio.begin{sample_rate, channels, bits, encoding: "f32le"}   before the first audio
 *   PCM frames                                                    float32 LE mono 24 kHz, whole samples, in order
 *   tts.failed                                                    speech stopped; the text carries on
 *   error{stage, message}                                         "request", "key", "network", "grounded"
 *   turn.end                                                      a turn that ran to its end
 *
 * A `cancel` frame from the client, or the client closing the connection, stops
 * the chat stream and the speech lane at once and no turn.end follows — Sewn's
 * barge-in. Errors before the turn starts (no key, no user message) end the
 * connection without a turn.end; a failed grounded pass reports and still ends
 * the turn, as Sewn's does. */
#ifndef MARY_SEWN_TURN_H
#define MARY_SEWN_TURN_H

#include <stddef.h>

#include "common/frame.h"
#include "sewn/server.h"

struct json_object;

/* API/GenerationDefaults.swift. */
#define SEWN_DEFAULT_MAX_TOKENS 128
#define SEWN_DEFAULT_TEMPERATURE 0.8
#define SEWN_DEFAULT_TOP_P 1.0
/* handleChat's verbatim-context shape keeps the ten most recent earlier turns. */
#define SEWN_HISTORY_TURNS 10

typedef struct sewn_turn_request {
    struct json_object *messages;   /* borrowed from the turn.start frame */
    const char *persona_name;       /* "Mary" when the request names none */
    const char *persona_voice;      /* NULL when absent */
    const char *instructions;       /* NULL when absent */
    const char *model;              /* a Mistral model (ModelConfig's prefixes), else SEWN_CHAT_MODEL */
    int max_tokens;
    double temperature;
    double top_p;
    const char *voice_id;           /* SEWN_TTS_VOICE when absent */
    const char *tts_model;          /* SEWN_TTS_MODEL when absent */
} sewn_turn_request;

/* 0, or -EINVAL when there is no request.messages array. */
int sewn_turn_request_parse(struct json_object *turn_start, sewn_turn_request *out);

/* The system message Sewn assembles for a turn with no retrieved context:
 * chatPersonaSection (Core/Personality.swift) with memoryInstruction(contextEmpty:)
 * (Core/Commands/Sewn+Compact.swift), then the conversational instructions and
 * base rules (Core/Sewn.swift handleChat). Heap; the caller frees. */
char *sewn_turn_system_prompt(const sewn_turn_request *req);

/* What goes to Mistral: up to SEWN_HISTORY_TURNS earlier user/assistant turns,
 * the latest user message, then the system message. NULL when the request holds
 * no user message. Sewn sends no history when nothing was retrieved; sewnd never
 * retrieves, so it keeps the verbatim-context shape (PORTING.md deviation 5). */
struct json_object *sewn_turn_messages(const sewn_turn_request *req);

/* Runs the turn named by `turn_start` on fd. `reader` holds whatever the client
 * sent after its first frame; the cancel watcher reads on through it. */
int sewn_run_turn(sewn_service *svc, int fd, mc_frame_reader *reader, struct json_object *turn_start);

#endif
