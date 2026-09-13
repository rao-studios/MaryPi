/* Mistral's voices, and one text spoken in any of them: what System Settings' voice picker lists and plays as a
 * sample (Mary's VoiceCharacter on macOS). Both are sewnd operations, one per connection:
 *
 *   voices.list                    → voices{voices: [{voice_id, id, name, languages[], gender?, custom}]}
 *                                   | error{stage: "key" | "network" | "voices", message}
 *   speak{voice_id, text, model?}  → audio.begin{sample_rate, channels, bits, encoding}, PCM frames, speak.end
 *                                   | tts.failed{status, message}, speak.end
 *                                   | error{stage: "request" | "key" | "network", message}   (before anything is spoken)
 *
 * voices.list reads GET /v1/audio/voices?type=all a hundred at a time, at most five pages. A voice is spoken by its
 * slug (fr_marie_neutral) when it has one and by its id otherwise; `custom` is a voice of the account's own. speak
 * takes a voice id of letters, digits, '_' and '-', and at most SEWN_SPEAK_TEXT_MAX bytes of text; a cancel frame,
 * or the client closing the connection, stops it. */
#ifndef MARY_SEWN_VOICES_H
#define MARY_SEWN_VOICES_H

#include <stddef.h>
#include <stdint.h>

#include "common/frame.h"
#include "sewn/server.h"

struct json_object;

#define SEWN_VOICES_PATH "/v1/audio/voices"
#define SEWN_VOICES_PAGE 100
#define SEWN_VOICES_PAGES_MAX 5
#define SEWN_VOICES_BODY_MAX (1u << 20)
#define SEWN_SPEAK_TEXT_MAX 500

/* One page of the list, its voices appended to `into` (a JSON array) as voices.list sends them; a voice whose id
 * sewnd could not send back is left out. *total is the page's `total`, 0 when it has none. 0, or -EINVAL. */
int sewn_voices_parse(const char *json, size_t len, struct json_object *into, int64_t *total);
/* 1..63 of [A-Za-z0-9_-]. */
int sewn_voice_id_valid(const char *voice_id);

int sewn_run_voices(sewn_service *svc, int fd);
int sewn_run_speak(sewn_service *svc, int fd, mc_frame_reader *reader, struct json_object *request);

#endif
