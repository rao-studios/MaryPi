/* One text spoken by Mistral: POST /v1/audio/speech, and the PCM streamed back as it arrives
 * (MistralTTS.stream in Sewn's Utilities/MistralTTSStream.swift). sewnd's turn lane speaks each sentence
 * with it. Nothing is lost quietly: a refusal carries Mistral's own reason, and an answer with no audio,
 * or with an event too large to read, is a failure too. Neither the key nor the text is logged here. */
#ifndef MARY_SEWN_SPEECH_H
#define MARY_SEWN_SPEECH_H

#include <stdbool.h>
#include <stddef.h>

#include "sewn/server.h"

/* How long one speech event may be: base64 of about a minute of audio. */
#define SEWN_SPEECH_LINE_MAX (8u << 20)

/* Whole float32 LE samples, 24 kHz mono, in order. Return nonzero to stop (the client has gone). */
typedef int (*sewn_pcm_fn)(const unsigned char *pcm, size_t len, void *user);

typedef struct sewn_speech {
    const char *model;          /* NULL: SEWN_TTS_MODEL */
    const char *voice_id;       /* NULL: SEWN_TTS_VOICE */
    const char *text;
} sewn_speech;

/* 0 when audio was spoken; -EPROTO when Mistral refused (*status, and `message` in words); -EMSGSIZE for an
 * event too large to read; -ENODATA when the answer carried no audio; -EIO when Mistral could not be
 * reached; -ECANCELED when should_stop or on_pcm ended it; -EINVAL for audio that would not decode;
 * -ENOMEM. `message` says why for every failure but a cancel, and never repeats the key or the text. */
int sewn_speak(const sewn_service *svc, const char *key, const sewn_speech *speech, sewn_pcm_fn on_pcm,
               sewn_stop_fn should_stop, void *user, long *status, char *message, size_t cap);

/* Mistral's own reason in an error body (`message`, `detail`, `detail[0].msg`, `error.message`, or plain text that is
 * not a page), on one line and trimmed; "" when there is none. */
void sewn_mistral_reason(const char *body, size_t len, char *out, size_t cap);

/* A refusal in words, from its status and the body that came with it: Mistral's own `message`, `detail`,
 * `detail[0].msg` or `error.message`, on one line and trimmed. 401 is the key. 403 is Mistral refusing to
 * speak the text (moderation, or an account without speech), never the key. */
void sewn_speech_failure(long status, const char *body, size_t len, char *out, size_t cap);

#endif
