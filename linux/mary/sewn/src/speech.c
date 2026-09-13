#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "sewn/speech.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "common/buf.h"
#include "common/json.h"
#include "common/sse.h"
#include "sewn/mistral.h"

#define FAILURE_BODY_MAX 2048
#define REASON_MAX 160

struct speaking {
    sewn_pcm_fn on_pcm;
    sewn_stop_fn should_stop;
    void *user;
    mc_sse_parser sse;
    mc_buf pcm;                 /* decoded, short of a whole sample */
    mc_buf failure;             /* a refusal's body */
    size_t spoken;              /* bytes handed on */
    bool done, stopped;
    int error;
};

/* The transport polls this with the speaking state; on_pcm refusing more counts too. */
static bool stop_now(void *arg) {
    struct speaking *k = arg;
    return k->stopped || (k->should_stop && k->should_stop(k->user));
}

static int on_event(const char *event, const char *data, size_t len, void *arg) {
    struct speaking *k = arg;
    int rc = sewn_speech_event(event, data, len, &k->pcm);
    if (rc == SEWN_SPEECH_AUDIO) {
        size_t whole = k->pcm.len - k->pcm.len % 4;
        if (!whole) return 0;
        if (k->on_pcm(k->pcm.data, whole, k->user)) {
            k->stopped = true;
            return 1;
        }
        k->spoken += whole;
        mc_buf_consume(&k->pcm, whole);
        return 0;
    }
    if (rc == SEWN_SPEECH_DONE) {
        k->done = true;
        return 1;
    }
    if (rc < 0) {
        k->error = rc;
        return 1;
    }
    return 0;
}

static int on_bytes(const char *bytes, size_t len, long status, void *arg) {
    struct speaking *k = arg;
    if (status && (status < 200 || status > 299)) {
        size_t room = FAILURE_BODY_MAX - k->failure.len;
        if (room) mc_buf_append(&k->failure, bytes, len < room ? len : room);
        return 0;
    }
    int rc = mc_sse_feed(&k->sse, bytes, len, on_event, k);
    if (rc < 0 && !k->error) k->error = rc;         /* an event too large to read is a failure, not the end */
    return rc != 0;
}

/* One line of printable text, at most REASON_MAX bytes, never cut inside a character. */
static void clean(const char *in, size_t len, char *out, size_t cap) {
    size_t n = 0;
    bool gap = false;
    for (size_t i = 0; in && i < len && in[i] && n + 2 < cap && n < REASON_MAX; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c <= ' ' || c == 0x7f) {
            gap = n > 0;
            continue;
        }
        if (gap) out[n++] = ' ';
        gap = false;
        out[n++] = (char)c;
    }
    size_t start = n;
    while (start > 0 && ((unsigned char)out[start - 1] & 0xC0) == 0x80) start--;
    if (start > 0) {
        unsigned char lead = (unsigned char)out[start - 1];
        size_t need = (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
        if (n - (start - 1) < need) n = start - 1;
    }
    if (cap) out[n] = 0;
}

static const char *reason_of(struct json_object *obj) {
    const char *said = mc_json_string(obj, "message");
    if (!said) said = mc_json_string(obj, "detail");
    if (!said) {
        struct json_object *detail = mc_json_array(obj, "detail");
        struct json_object *first = detail && json_object_array_length(detail) ? json_object_array_get_idx(detail, 0) : NULL;
        said = mc_json_string(first, "msg");
    }
    if (!said) said = mc_json_string(mc_json_object(obj, "error"), "message");
    return said;
}

void sewn_mistral_reason(const char *body, size_t len, char *out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    struct json_object *obj = body && len ? mc_json_parse(body, len) : NULL;
    const char *said = reason_of(obj);
    if (said) clean(said, strlen(said), out, cap);
    else if (body && len && body[0] != '<' && body[0] != '{') clean(body, len, out, cap);   /* plain text, not a page */
    json_object_put(obj);
}

void sewn_speech_failure(long status, const char *body, size_t len, char *out, size_t cap) {
    if (!cap) return;
    if (status == 401) {
        snprintf(out, cap, "Mistral rejected the key");
        return;
    }
    if (status == 429) {
        snprintf(out, cap, "Mistral is rate-limiting this key");
        return;
    }
    char reason[REASON_MAX + 1];
    sewn_mistral_reason(body, len, reason, sizeof reason);
    if (status == 403 && reason[0]) snprintf(out, cap, "Mistral refused to speak it (HTTP 403: %s)", reason);
    else if (status == 403) snprintf(out, cap, "Mistral refused to speak it (HTTP 403)");
    else if (reason[0]) snprintf(out, cap, "Mistral answered HTTP %ld: %s", status, reason);
    else snprintf(out, cap, "Mistral answered HTTP %ld", status);
}

int sewn_speak(const sewn_service *svc, const char *key, const sewn_speech *speech, sewn_pcm_fn on_pcm,
               sewn_stop_fn should_stop, void *user, long *status, char *message, size_t cap) {
    *status = 0;
    if (cap) message[0] = 0;
    if (!svc->post_stream) {
        snprintf(message, cap, "sewnd was built without libcurl");
        return -EIO;
    }
    struct json_object *body = sewn_speech_body(speech->model, speech->text, speech->voice_id);
    size_t body_len = 0;
    const char *text = body ? mc_json_compact(body, &body_len) : NULL;
    if (!text) {
        json_object_put(body);
        snprintf(message, cap, "out of memory");
        return -ENOMEM;
    }
    struct speaking k = { .on_pcm = on_pcm, .should_stop = should_stop, .user = user };
    mc_sse_init(&k.sse, svc->speech_line_max ? svc->speech_line_max : SEWN_SPEECH_LINE_MAX);
    k.sse.bare_json = true;                         /* MistralTTS's NDJSON fallback */
    char transport[256] = "";
    int rc = svc->post_stream(SEWN_MISTRAL_SPEECH_PATH, key, text, body_len, on_bytes, stop_now, &k, status, transport,
                              sizeof transport, svc->post_stream_user);
    json_object_put(body);
    bool refused = *status && (*status < 200 || *status > 299);
    if (rc >= 0 && !refused && !k.done && !k.error && !stop_now(&k)) {
        int end = mc_sse_finish(&k.sse, on_event, &k);
        if (end < 0 && !k.error) k.error = end;
    }
    int result = 0;
    if (rc == -ECANCELED || stop_now(&k)) {
        result = -ECANCELED;
    } else if (refused) {
        sewn_speech_failure(*status, (const char *)k.failure.data, k.failure.len, message, cap);
        result = -EPROTO;
    } else if (rc < 0) {
        snprintf(message, cap, "%s", transport[0] ? transport : "Mistral could not be reached");
        result = -EIO;
    } else if (k.error == -EMSGSIZE) {
        snprintf(message, cap, "Mistral's speech came in a piece too large to read");
        result = -EMSGSIZE;
    } else if (k.error) {
        snprintf(message, cap, "Mistral's speech could not be read");
        result = k.error;
    } else if (!k.spoken) {
        snprintf(message, cap, "Mistral answered with no audio");
        result = -ENODATA;
    }
    /* Whatever came back, the reason never repeats the key or the words. */
    bool echoes = cap && ((key && *key && strstr(message, key)) || (speech->text && *speech->text && strstr(message, speech->text)));
    if (result < 0 && result != -ECANCELED && echoes) snprintf(message, cap, "Mistral answered HTTP %ld", *status);
    mc_sse_free(&k.sse);
    mc_buf_free(&k.pcm);
    mc_buf_free(&k.failure);
    return result;
}
