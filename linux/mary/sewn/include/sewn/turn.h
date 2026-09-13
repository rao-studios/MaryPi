/* sewnd's turn: Sewn's realtime route (API/Routes/Realtime/Realtime.swift,
 * RealtimeTurnEngine.swift) as one grounded pass with Sewn's handleChat around it
 * (Core/Sewn.swift): retrieval from the Thread in the lanes the request names,
 * compaction, the citation-marker protocol, generation, span attribution and
 * auto-memory. The connection's first frame is `turn.start{request, tts{voice_id,
 * model}}`, where `request` is Mary's ChatRequest — the JSON Sewn accepts, with
 * `provider` and `sewn{owner_id, lanes[], groups[], entities[], request_id}` — and
 * sewnd answers on the same connection:
 *
 *   phase{phase: "grounded"}                                      once
 *   retrieval{lanes, partitions: [{document_id, name, family, lane, group_id, score}], ms}
 *                                                                 once, when something was searched
 *   token{phase: "grounded", text}                                as Mistral streams, [[n]] markers stripped
 *   audio.begin{sample_rate, channels, bits, encoding: "f32le"}   before the first audio
 *   PCM frames                                                    float32 LE mono 24 kHz, whole samples, in order
 *   tts.failed{status, message}                                   speech stopped, with Mistral's reason; the text carries on
 *   error{stage, message}                                         "request", "engine", "key", "network", "grounded"
 *   turn.end{text, contribution, retrieved, memory?}              a turn that ran to its end: the visible reply,
 *                                                                 Gita's contribution with spans, what was retrieved
 *
 * A `cancel` frame from the client, or the client closing the connection, stops
 * the chat stream and the speech lane at once and no turn.end follows — Sewn's
 * barge-in. Errors before the turn starts (no key, no user message, an engine that
 * is not served) end the connection without a turn.end; a failed grounded pass
 * reports and still ends the turn, as Sewn's does. The owner of the retrieval and
 * of the memory is the connection's user, never the request's owner_id. */
#ifndef MARY_SEWN_TURN_H
#define MARY_SEWN_TURN_H

#include <stdbool.h>
#include <stddef.h>

#include "common/frame.h"
#include "sewn/mistral.h"
#include "sewn/provider.h"
#include "sewn/retrieve.h"
#include "sewn/server.h"

struct json_object;

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
    sewn_provider provider;
    bool provider_known;            /* false: the name was not one sewnd knows */
    sewn_scope scope;               /* request.sewn */
    const char *recent;             /* the latest user message's content, or NULL */
    int user_messages;              /* how many user messages the request holds */
} sewn_turn_request;

/* 0, or -EINVAL when there is no request.messages array. */
int sewn_turn_request_parse(struct json_object *turn_start, sewn_turn_request *out);

/* The system message Sewn assembles: chatPersonaSection (Core/Personality.swift) with
 * memoryInstruction(contextEmpty:) (Core/Commands/Sewn+Compact.swift), the
 * conversational instructions and base rules (Core/Sewn.swift handleChat), then the
 * context block when there is one. Heap; the caller frees. */
char *sewn_turn_system_prompt_with(const sewn_turn_request *req, const char *context);
/* The same with no retrieved context. */
char *sewn_turn_system_prompt(const sewn_turn_request *req);
/* The --- CONTEXT --- block around compacted text (contextUsageGuide + the citation protocol). Heap. */
char *sewn_turn_context_block(const char *compacted);

/* What goes to Mistral: up to `history_turns` earlier user/assistant turns, the latest
 * user message, then the system message. NULL when the request holds no user message. */
struct json_object *sewn_turn_messages_with(const sewn_turn_request *req, const char *system, int history_turns);
/* The no-context shape: ten turns of history and sewn_turn_system_prompt. */
struct json_object *sewn_turn_messages(const sewn_turn_request *req);
/* The earlier turns as [{role, content}] excluding the latest user message (compaction's history). */
struct json_object *sewn_turn_history(const sewn_turn_request *req);

/* Runs the turn named by `turn_start` on fd for `owner` (the connection's user).
 * `reader` holds whatever the client sent after its first frame; the cancel watcher
 * reads on through it. */
int sewn_run_turn(sewn_service *svc, int fd, mc_frame_reader *reader, struct json_object *turn_start, const char *owner);

#endif
