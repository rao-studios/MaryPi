/* Mistral's wire as sewnd speaks it: request bodies built, streamed events read.
 * No I/O lives here — sewnd's transports carry the bytes — so everything is
 * tested against recorded payloads. The host is fixed: a configurable base URL
 * would be a way to send the key somewhere else. */
#ifndef MARY_SEWN_MISTRAL_H
#define MARY_SEWN_MISTRAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/buf.h"

struct json_object;

#define SEWN_MISTRAL_HOST "api.mistral.ai"
#define SEWN_MISTRAL_CHAT_PATH "/v1/chat/completions"
#define SEWN_MISTRAL_SPEECH_PATH "/v1/audio/speech"
#define SEWN_MISTRAL_REALTIME_PATH "/v1/audio/transcriptions/realtime"

#define SEWN_CHAT_MODEL "mistral-medium-latest"             /* ModelConfig: SEWN_CHAT_MODEL's default */
#define SEWN_TTS_MODEL "voxtral-mini-tts-2603"               /* MistralTTS.defaultModel */
#define SEWN_TTS_VOICE "fr_marie_neutral"                    /* Realtime.swift's default voice */
#define SEWN_TTS_SAMPLE_RATE 24000                           /* float32 LE mono */
#define SEWN_STT_MODEL "voxtral-mini-transcribe-realtime-2602"
#define SEWN_STT_SAMPLE_RATE 16000                           /* pcm_s16le mono */
#define SEWN_STT_MAX_APPEND 262144                           /* decoded bytes per input_audio.append */

/* MARK: - Chat (runStreamMistral in Providers/ModelProvider+Stream.swift) */

/* {model, messages, max_tokens, temperature, top_p, stream: true} — exactly what
 * Sewn sends; Mistral has no repetition penalty. Takes a reference to `messages`. */
struct json_object *sewn_chat_body(const char *model, struct json_object *messages, int max_tokens,
                                   double temperature, double top_p);

typedef enum sewn_chat_event_kind {
    SEWN_CHAT_IGNORED,      /* unparseable, or no choices: Sewn's handle(payload:) skips it */
    SEWN_CHAT_DELTA,
    SEWN_CHAT_DONE,         /* "[DONE]" */
} sewn_chat_event_kind;

typedef struct sewn_chat_event {
    sewn_chat_event_kind kind;
    const char *content;    /* DELTA: the text, possibly NULL (a role-only delta); owned by holder */
    bool finished;          /* DELTA: finish_reason was set — Sewn stops there too */
    struct json_object *holder;
} sewn_chat_event;

/* One `data:` payload of the chat stream. Release with sewn_chat_event_release. */
void sewn_chat_event_parse(const char *data, size_t len, sewn_chat_event *out);
void sewn_chat_event_release(sewn_chat_event *event);

/* MARK: - Speech (Utilities/MistralTTSStream.swift) */

/* {model, input, voice_id, response_format: "pcm", stream: true}. */
struct json_object *sewn_speech_body(const char *model, const char *input, const char *voice_id);

#define SEWN_SPEECH_AUDIO 1
#define SEWN_SPEECH_DONE 2

/* One event of the speech stream, read as MistralTTS.extractPCM reads it: a
 * top-level {"audio_data": b64}, or {"event": "speech.audio.delta", "data":
 * {"audio_data": b64}}. Decoded PCM is appended to `pcm`. SEWN_SPEECH_AUDIO,
 * 0 when the event carried no audio, SEWN_SPEECH_DONE for "[DONE]" or
 * speech.audio.done, -EINVAL for bad base64, or -ENOMEM. `sse_event` is the
 * SSE `event:` name ("" when there was none). */
int sewn_speech_event(const char *sse_event, const char *data, size_t len, mc_buf *pcm);

/* MARK: - Voxtral Realtime (mistralai client-python, client/models/realtimetranscription*) */

/* session.update: set before any audio; a delay <= 0 is left to the server. */
struct json_object *sewn_stt_session_update(int sample_rate, int target_streaming_delay_ms);
/* input_audio.append for n <= SEWN_STT_MAX_APPEND bytes of pcm_s16le; NULL when
 * n is larger (errno EMSGSIZE) or memory runs out. */
struct json_object *sewn_stt_append(const unsigned char *pcm, size_t n);
struct json_object *sewn_stt_flush(void);
struct json_object *sewn_stt_end(void);

typedef enum sewn_stt_event_kind {
    SEWN_STT_UNKNOWN,
    SEWN_STT_SESSION,       /* session.created, session.updated */
    SEWN_STT_TEXT_DELTA,    /* text */
    SEWN_STT_SEGMENT,       /* text, when the segment carries it */
    SEWN_STT_LANGUAGE,      /* text: the language */
    SEWN_STT_DONE,          /* text: the whole transcript */
    SEWN_STT_ERROR,         /* text: the message (an object message is serialized); code */
} sewn_stt_event_kind;

typedef struct sewn_stt_event {
    sewn_stt_event_kind kind;
    const char *text;       /* owned by holder */
    int64_t code;
    struct json_object *holder;
} sewn_stt_event;

/* 0, or -EINVAL when `json` is not a JSON object with a string "type". */
int sewn_stt_event_parse(const char *json, size_t len, sewn_stt_event *out);
void sewn_stt_event_release(sewn_stt_event *event);

#endif
