#include "sewn/mistral.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/base64.h"
#include "common/json.h"

/* A double that serializes as written ("0.4", not "0.40000000000000002"). */
static struct json_object *number(double value) {
    char text[32];
    snprintf(text, sizeof text, "%.15g", value);
    return json_object_new_double_s(value, text);
}

/* MARK: - Chat */

struct json_object *sewn_chat_body(const char *model, struct json_object *messages, int max_tokens,
                                   double temperature, double top_p) {
    struct json_object *body = json_object_new_object();
    if (!body) return NULL;
    json_object_object_add(body, "model", json_object_new_string(model ? model : SEWN_CHAT_MODEL));
    json_object_object_add(body, "messages", json_object_get(messages));
    json_object_object_add(body, "max_tokens", json_object_new_int(max_tokens));
    json_object_object_add(body, "temperature", number(temperature));
    json_object_object_add(body, "top_p", number(top_p));
    json_object_object_add(body, "stream", json_object_new_boolean(1));
    return body;
}

void sewn_chat_event_parse(const char *data, size_t len, sewn_chat_event *out) {
    memset(out, 0, sizeof *out);
    if (len == 6 && memcmp(data, "[DONE]", 6) == 0) {
        out->kind = SEWN_CHAT_DONE;
        return;
    }
    struct json_object *chunk = mc_json_parse(data, len);
    struct json_object *choices = mc_json_array(chunk, "choices");
    struct json_object *choice = choices && json_object_array_length(choices) ? json_object_array_get_idx(choices, 0) : NULL;
    if (!choice || !json_object_is_type(choice, json_type_object)) {
        json_object_put(chunk);
        return;
    }
    out->kind = SEWN_CHAT_DELTA;
    out->holder = chunk;
    out->content = mc_json_string(mc_json_object(choice, "delta"), "content");
    struct json_object *finish;
    out->finished = json_object_object_get_ex(choice, "finish_reason", &finish) && finish != NULL;
}

void sewn_chat_event_release(sewn_chat_event *event) {
    json_object_put(event->holder);
    memset(event, 0, sizeof *event);
}

/* MARK: - Speech */

struct json_object *sewn_speech_body(const char *model, const char *input, const char *voice_id) {
    struct json_object *body = json_object_new_object();
    if (!body) return NULL;
    json_object_object_add(body, "model", json_object_new_string(model ? model : SEWN_TTS_MODEL));
    json_object_object_add(body, "input", json_object_new_string(input ? input : ""));
    json_object_object_add(body, "voice_id", json_object_new_string(voice_id ? voice_id : SEWN_TTS_VOICE));
    json_object_object_add(body, "response_format", json_object_new_string("pcm"));
    json_object_object_add(body, "stream", json_object_new_boolean(1));
    return body;
}

static int append_audio(const char *b64, mc_buf *pcm) {
    size_t n = strlen(b64), got = 0;
    if (mc_buf_reserve(pcm, mc_base64_decoded_max(n)) < 0) return -ENOMEM;
    int rc = mc_base64_decode(b64, n, pcm->data + pcm->len, pcm->cap - pcm->len - 1, &got);
    if (rc < 0) return rc;
    pcm->len += got;
    pcm->data[pcm->len] = 0;
    return got ? SEWN_SPEECH_AUDIO : 0;
}

int sewn_speech_event(const char *sse_event, const char *data, size_t len, mc_buf *pcm) {
    static const char DELTA[] = "speech.audio.delta", DONE[] = "speech.audio.done";
    if (len == 6 && memcmp(data, "[DONE]", 6) == 0) return SEWN_SPEECH_DONE;
    if (sse_event && strcmp(sse_event, DONE) == 0) return SEWN_SPEECH_DONE;
    struct json_object *obj = mc_json_parse(data, len);
    if (!obj) return 0;
    int rc = 0;
    const char *b64 = mc_json_string(obj, "audio_data");
    const char *inner = mc_json_string(obj, "event");
    if (b64) {
        rc = append_audio(b64, pcm);
    } else if (inner && strcmp(inner, DONE) == 0) {
        rc = SEWN_SPEECH_DONE;
    } else if (inner) {
        /* requireDelta: under a delta event (or none), only an inner delta counts. */
        bool require_delta = !sse_event || !*sse_event || strcmp(sse_event, DELTA) == 0;
        const char *nested = mc_json_string(mc_json_object(obj, "data"), "audio_data");
        if (nested && (!require_delta || strcmp(inner, DELTA) == 0)) rc = append_audio(nested, pcm);
    }
    json_object_put(obj);
    return rc;
}

/* MARK: - Voxtral Realtime */

static struct json_object *typed(const char *type) {
    struct json_object *msg = json_object_new_object();
    if (msg) json_object_object_add(msg, "type", json_object_new_string(type));
    return msg;
}

struct json_object *sewn_stt_session_update(int sample_rate, int target_streaming_delay_ms) {
    struct json_object *msg = typed("session.update");
    if (!msg) return NULL;
    struct json_object *session = json_object_new_object(), *format = json_object_new_object();
    json_object_object_add(format, "encoding", json_object_new_string("pcm_s16le"));
    json_object_object_add(format, "sample_rate", json_object_new_int(sample_rate > 0 ? sample_rate : SEWN_STT_SAMPLE_RATE));
    json_object_object_add(session, "audio_format", format);
    if (target_streaming_delay_ms > 0)
        json_object_object_add(session, "target_streaming_delay_ms", json_object_new_int(target_streaming_delay_ms));
    json_object_object_add(msg, "session", session);
    return msg;
}

struct json_object *sewn_stt_append(const unsigned char *pcm, size_t n) {
    if (n > SEWN_STT_MAX_APPEND) {
        errno = EMSGSIZE;
        return NULL;
    }
    size_t len = 0;
    char *b64 = mc_base64_encode_alloc(pcm, n, &len);
    struct json_object *msg = b64 ? typed("input_audio.append") : NULL;
    if (msg) json_object_object_add(msg, "audio", json_object_new_string_len(b64, (int)len));
    free(b64);
    return msg;
}

struct json_object *sewn_stt_flush(void) { return typed("input_audio.flush"); }
struct json_object *sewn_stt_end(void) { return typed("input_audio.end"); }

int sewn_stt_event_parse(const char *json, size_t len, sewn_stt_event *out) {
    memset(out, 0, sizeof *out);
    struct json_object *obj = mc_json_parse(json, len);
    const char *type = mc_json_type(obj);
    if (!type) {
        json_object_put(obj);
        return -EINVAL;
    }
    out->holder = obj;
    if (strcmp(type, "session.created") == 0 || strcmp(type, "session.updated") == 0) {
        out->kind = SEWN_STT_SESSION;
    } else if (strcmp(type, "transcription.text.delta") == 0) {
        out->kind = SEWN_STT_TEXT_DELTA;
        out->text = mc_json_string(obj, "text");
    } else if (strcmp(type, "transcription.segment") == 0) {
        out->kind = SEWN_STT_SEGMENT;
        out->text = mc_json_string(obj, "text");
    } else if (strcmp(type, "transcription.language") == 0) {
        out->kind = SEWN_STT_LANGUAGE;
        out->text = mc_json_string(obj, "audio_language");
        if (!out->text) out->text = mc_json_string(obj, "language");
    } else if (strcmp(type, "transcription.done") == 0) {
        out->kind = SEWN_STT_DONE;
        out->text = mc_json_string(obj, "text");
    } else if (strcmp(type, "error") == 0) {
        out->kind = SEWN_STT_ERROR;
        struct json_object *detail = mc_json_object(obj, "error"), *message;
        mc_json_int64(detail, "code", &out->code);
        if (detail && json_object_object_get_ex(detail, "message", &message))
            out->text = json_object_is_type(message, json_type_string) ? json_object_get_string(message) : mc_json_compact(message, NULL);
    } else {
        out->kind = SEWN_STT_UNKNOWN;
    }
    return 0;
}

void sewn_stt_event_release(sewn_stt_event *event) {
    json_object_put(event->holder);
    memset(event, 0, sizeof *event);
}
