#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "sewn/turn.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/io.h"
#include "common/json.h"
#include "common/log.h"
#include "common/secure.h"
#include "common/sse.h"
#include "sewn/chunker.h"
#include "sewn/mistral.h"

/* Core/Sewn.swift handleChat. */
static const char BASE_RULES[] =
    "Keep responses under 6-7 sentences. Be specific and grounded. Never announce that you are an AI. "
    "Never output XML-like tags (such as <external>) in your response.";
/* Core/Commands/Sewn+Compact.swift memoryInstruction(contextEmpty: true). */
static const char NO_CONTEXT_MEMORY[] =
    "You have no retrieved memories or documents for this user. Do not reference, invent, or imply knowledge "
    "of any past conversations, notes, or memories \xE2\x80\x94 respond only from what the user tells you directly "
    "in this conversation.";

#define SSE_LINE_MAX (1u << 20)
#define FAILURE_BODY_MAX 2048

/* MARK: - The request */

/* Core/ModelConfig.swift: a model counts as Mistral's by its name. */
static bool is_mistral_model(const char *name) {
    static const char *const prefixes[] = { "mistral", "open-mi", "ministral", "codestral" };
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++)
        if (strncmp(name, prefixes[i], strlen(prefixes[i])) == 0) return true;
    return false;
}

static const char *nonempty(const char *s, const char *fallback) { return s && *s ? s : fallback; }

int sewn_turn_request_parse(struct json_object *turn_start, sewn_turn_request *out) {
    memset(out, 0, sizeof *out);
    struct json_object *request = mc_json_object(turn_start, "request");
    out->messages = mc_json_array(request, "messages");
    if (!out->messages) return -EINVAL;
    struct json_object *persona = mc_json_object(request, "persona");
    out->persona_name = nonempty(mc_json_string(persona, "name"), "Mary");
    out->persona_voice = mc_json_string(persona, "voice");
    out->instructions = mc_json_string(request, "instructions");
    const char *model = mc_json_string(request, "model");
    out->model = model && is_mistral_model(model) ? model : SEWN_CHAT_MODEL;
    int64_t max_tokens = 0;
    out->max_tokens = mc_json_int64(request, "max_tokens", &max_tokens) && max_tokens > 0 && max_tokens <= 32768
                          ? (int)max_tokens : SEWN_DEFAULT_MAX_TOKENS;
    out->temperature = SEWN_DEFAULT_TEMPERATURE;
    if (mc_json_double(request, "temperature", &out->temperature) && (out->temperature < 0 || out->temperature > 2))
        out->temperature = SEWN_DEFAULT_TEMPERATURE;
    out->top_p = SEWN_DEFAULT_TOP_P;
    if (mc_json_double(request, "top_p", &out->top_p) && (out->top_p <= 0 || out->top_p > 1))
        out->top_p = SEWN_DEFAULT_TOP_P;
    struct json_object *tts = mc_json_object(turn_start, "tts");
    out->voice_id = nonempty(mc_json_string(tts, "voice_id"), SEWN_TTS_VOICE);
    out->tts_model = nonempty(mc_json_string(tts, "model"), SEWN_TTS_MODEL);
    return 0;
}

char *sewn_turn_system_prompt(const sewn_turn_request *req) {
    mc_buf b = { 0 };
    int rc = 0;
    rc |= mc_buf_append_str(&b, "Your name is ");
    rc |= mc_buf_append_str(&b, req->persona_name ? req->persona_name : "Mary");
    rc |= mc_buf_append_str(&b, ".\n\n");
    if (req->persona_voice && *req->persona_voice) {
        rc |= mc_buf_append_str(&b, req->persona_voice);
        rc |= mc_buf_append_str(&b, " ");
    }
    rc |= mc_buf_append_str(&b, NO_CONTEXT_MEMORY);
    rc |= mc_buf_append_str(&b, "\n\n");
    if (req->instructions && *req->instructions) {
        rc |= mc_buf_append_str(&b, "--- CONVERSATIONAL INSTRUCTIONS ---\n");
        rc |= mc_buf_append_str(&b, req->instructions);
        rc |= mc_buf_append_str(&b, "\n\n");
    }
    rc |= mc_buf_append_str(&b, BASE_RULES);
    rc |= mc_buf_append_str(&b, "\n\n");   /* then the context, which is empty */
    if (rc) {
        mc_buf_free(&b);
        return NULL;
    }
    return (char *)b.data;
}

static struct json_object *role_content(const char *role, const char *content) {
    struct json_object *m = json_object_new_object();
    json_object_object_add(m, "role", json_object_new_string(role));
    json_object_object_add(m, "content", json_object_new_string(content));
    return m;
}

struct json_object *sewn_turn_messages(const sewn_turn_request *req) {
    size_t n = json_object_array_length(req->messages);
    size_t *usable = calloc(n ? n : 1, sizeof *usable), count = 0;
    long last_user = -1;
    if (!usable) return NULL;
    for (size_t i = 0; i < n; i++) {
        struct json_object *m = json_object_array_get_idx(req->messages, i);
        const char *role = mc_json_string(m, "role"), *content = mc_json_string(m, "content");
        if (!role || !content || !*content || (strcmp(role, "user") && strcmp(role, "assistant"))) continue;
        if (strcmp(role, "user") == 0) last_user = (long)count;
        usable[count++] = i;
    }
    char *system = last_user >= 0 ? sewn_turn_system_prompt(req) : NULL;
    if (!system) {
        free(usable);
        return NULL;
    }
    struct json_object *out = json_object_new_array();
    long first = last_user > SEWN_HISTORY_TURNS ? last_user - SEWN_HISTORY_TURNS : 0;
    for (long k = first; k <= last_user; k++) {
        struct json_object *m = json_object_array_get_idx(req->messages, usable[k]);
        json_object_array_add(out, role_content(mc_json_string(m, "role"), mc_json_string(m, "content")));
    }
    json_object_array_add(out, role_content("system", system));
    free(system);
    free(usable);
    return out;
}

/* MARK: - The turn */

struct turn {
    sewn_service *svc;
    int fd;
    mc_frame_reader *reader;
    const sewn_turn_request *req;
    char key[SEWN_KEY_MAX + 1];
    pthread_mutex_t write_lock;
    atomic_bool cancelled;
    atomic_bool finished;
    /* The speech lane: one worker, so audio frames go out in sentence order. */
    pthread_mutex_t lane_lock;
    pthread_cond_t lane_ready;
    char **queue;
    size_t head, count, cap;
    bool no_more;
    bool audio_begun;           /* under write_lock */
    /* For the log line. */
    int64_t started_ms, first_token_ms, first_audio_ms;
    size_t tokens, sentences;
};


static int write_locked(struct turn *t, struct json_object *msg) {
    int rc = mc_frame_write_json(t->fd, msg);
    json_object_put(msg);
    if (rc < 0) atomic_store(&t->cancelled, true);   /* the client has gone */
    return rc;
}

static int send_json(struct turn *t, struct json_object *msg) {
    pthread_mutex_lock(&t->write_lock);
    int rc = write_locked(t, msg);
    pthread_mutex_unlock(&t->write_lock);
    return rc;
}

static struct json_object *typed(const char *type) {
    struct json_object *msg = json_object_new_object();
    json_object_object_add(msg, "type", json_object_new_string(type));
    return msg;
}

static int send_error(struct turn *t, const char *stage, const char *message) {
    struct json_object *msg = typed("error");
    json_object_object_add(msg, "stage", json_object_new_string(stage));
    json_object_object_add(msg, "message", json_object_new_string(message));
    return send_json(t, msg);
}

static int send_token(struct turn *t, const char *text, size_t len) {
    struct json_object *msg = typed("token");
    json_object_object_add(msg, "phase", json_object_new_string("grounded"));
    json_object_object_add(msg, "text", json_object_new_string_len(text, (int)len));
    return send_json(t, msg);
}

/* Whole float32 samples only; audio.begin before the first. */
static int send_pcm(struct turn *t, const unsigned char *pcm, size_t len) {
    int rc = 0;
    pthread_mutex_lock(&t->write_lock);
    if (!t->audio_begun) {
        struct json_object *begin = typed("audio.begin");
        json_object_object_add(begin, "sample_rate", json_object_new_int(SEWN_TTS_SAMPLE_RATE));
        json_object_object_add(begin, "channels", json_object_new_int(1));
        json_object_object_add(begin, "bits", json_object_new_int(32));
        json_object_object_add(begin, "encoding", json_object_new_string("f32le"));
        rc = write_locked(t, begin);
        t->audio_begun = true;
        t->first_audio_ms = mc_now_ms() - t->started_ms;
    }
    size_t max = MC_FRAME_MAX - MC_FRAME_MAX % 4;
    for (size_t off = 0; rc == 0 && off < len; off += max) {
        size_t n = len - off < max ? len - off : max;
        rc = mc_frame_write_fd(t->fd, MC_FRAME_PCM, pcm + off, n);
        if (rc < 0) atomic_store(&t->cancelled, true);
    }
    pthread_mutex_unlock(&t->write_lock);
    return rc;
}

/* MARK: The client */

static int on_client_frame(uint8_t kind, const unsigned char *bytes, size_t len, void *user) {
    struct turn *t = user;
    if (kind != MC_FRAME_JSON) return 0;
    struct json_object *msg = mc_json_parse((const char *)bytes, len);
    const char *type = mc_json_type(msg);
    bool cancel = type && strcmp(type, "cancel") == 0;
    json_object_put(msg);
    if (!cancel) return 0;
    atomic_store(&t->cancelled, true);
    return 1;
}

static void *watch_client(void *arg) {
    struct turn *t = arg;
    if (mc_frame_reader_feed(t->reader, NULL, 0, on_client_frame, t) != 0) return NULL;   /* a cancel already waiting */
    while (!atomic_load(&t->finished) && !atomic_load(&t->cancelled)) {
        struct pollfd p = { .fd = t->fd, .events = POLLIN };
        int r = poll(&p, 1, 100);
        if (r < 0 && errno == EINTR) continue;
        if (r == 0) continue;
        int status = r < 0 ? -errno : mc_frame_reader_read_fd(t->reader, t->fd, on_client_frame, t);
        if (status == MC_IO_OK || status == -EAGAIN || status == -EWOULDBLOCK || status == -EINTR) continue;
        atomic_store(&t->cancelled, true);   /* cancel, end of stream, or a broken connection */
        break;
    }
    return NULL;
}

/* MARK: Speech */

static int enqueue(const char *chunk, size_t len, void *user) {
    struct turn *t = user;
    char *clean = sewn_tts_sanitize(chunk);
    if (!clean || !*clean) {
        free(clean);
        return 0;
    }
    pthread_mutex_lock(&t->lane_lock);
    if (t->head + t->count == t->cap) {
        if (t->head) {
            memmove(t->queue, t->queue + t->head, t->count * sizeof *t->queue);
            t->head = 0;
        } else {
            size_t cap = t->cap ? t->cap * 2 : 16;
            char **grown = realloc(t->queue, cap * sizeof *grown);
            if (!grown) {
                pthread_mutex_unlock(&t->lane_lock);
                free(clean);
                return 0;
            }
            t->queue = grown;
            t->cap = cap;
        }
    }
    t->queue[t->head + t->count++] = clean;
    t->sentences++;
    pthread_cond_signal(&t->lane_ready);
    pthread_mutex_unlock(&t->lane_lock);
    return 0;
}

struct speech {
    struct turn *t;
    mc_sse_parser sse;
    mc_buf pcm;
    bool done;
    int error;
};

static int on_speech_event(const char *event, const char *data, size_t len, void *user) {
    struct speech *s = user;
    int rc = sewn_speech_event(event, data, len, &s->pcm);
    if (rc == SEWN_SPEECH_AUDIO) {
        size_t whole = s->pcm.len - s->pcm.len % 4;
        if (whole && send_pcm(s->t, s->pcm.data, whole) < 0) return 1;
        mc_buf_consume(&s->pcm, whole);
        return 0;
    }
    if (rc == SEWN_SPEECH_DONE) {
        s->done = true;
        return 1;
    }
    if (rc < 0) {
        s->error = rc;
        return 1;
    }
    return 0;
}

/* The transport hands one pointer to both callbacks: the speech state, which knows its turn. */
static bool speech_should_stop(void *user) {
    struct speech *s = user;
    return atomic_load(&s->t->cancelled);
}

static int on_speech_bytes(const char *bytes, size_t len, long status, void *user) {
    struct speech *s = user;
    if (status && (status < 200 || status > 299)) return 1;
    return mc_sse_feed(&s->sse, bytes, len, on_speech_event, s) != 0;
}

static bool speak(struct turn *t, const char *sentence) {
    struct json_object *body = sewn_speech_body(t->req->tts_model, sentence, t->req->voice_id);
    size_t len = 0;
    const char *text = mc_json_compact(body, &len);
    struct speech s = { .t = t };
    mc_sse_init(&s.sse, SSE_LINE_MAX);
    char message[256] = "";
    long status = 0;
    int rc = t->svc->post_stream(SEWN_MISTRAL_SPEECH_PATH, t->key, text, len, on_speech_bytes, speech_should_stop, &s,
                                 &status, message, sizeof message, t->svc->post_stream_user);
    json_object_put(body);
    if (rc >= 0 && !s.done && !s.error && !atomic_load(&t->cancelled)) mc_sse_finish(&s.sse, on_speech_event, &s);
    bool ok = rc >= 0 && s.error == 0 && (status == 0 || (status >= 200 && status <= 299));
    if (!ok && !atomic_load(&t->cancelled)) {
        if (rc < 0) mc_log(MC_LOG_WARNING, "speech failed: %s", message);
        else mc_log(MC_LOG_WARNING, "speech failed: HTTP %ld", status);
    }
    mc_sse_free(&s.sse);
    mc_buf_free(&s.pcm);
    return ok;
}

static void *speech_lane(void *arg) {
    struct turn *t = arg;
    for (;;) {
        pthread_mutex_lock(&t->lane_lock);
        while (t->count == 0 && !t->no_more && !atomic_load(&t->cancelled)) pthread_cond_wait(&t->lane_ready, &t->lane_lock);
        if (atomic_load(&t->cancelled) || t->count == 0) {
            pthread_mutex_unlock(&t->lane_lock);
            break;
        }
        char *sentence = t->queue[t->head++];
        if (--t->count == 0) t->head = 0;
        pthread_mutex_unlock(&t->lane_lock);
        bool ok = speak(t, sentence);
        free(sentence);
        if (!ok) {
            /* Sewn's TTS lane ends at its first failure; the words keep coming. */
            if (!atomic_load(&t->cancelled)) {
                struct json_object *failed = typed("tts.failed");
                send_json(t, failed);
            }
            break;
        }
    }
    return NULL;
}

/* MARK: Chat */

struct chat {
    struct turn *t;
    mc_sse_parser sse;
    sewn_chunker chunker;
    mc_buf failure;
    bool finished;
};

static int on_chat_event(const char *event, const char *data, size_t len, void *user) {
    struct chat *c = user;
    sewn_chat_event e;
    sewn_chat_event_parse(data, len, &e);
    int stop = e.kind == SEWN_CHAT_DONE;
    if (e.kind == SEWN_CHAT_DELTA) {
        if (e.content && *e.content) {
            size_t n = strlen(e.content);
            if (!c->t->tokens++) c->t->first_token_ms = mc_now_ms() - c->t->started_ms;
            if (send_token(c->t, e.content, n) < 0) stop = 1;
            else sewn_chunker_feed(&c->chunker, e.content, n, enqueue, c->t);
        }
        if (e.finished) stop = 1;
    }
    sewn_chat_event_release(&e);
    if (stop) c->finished = true;
    return stop;
}

static bool chat_should_stop(void *user) {
    struct chat *c = user;
    return atomic_load(&c->t->cancelled);
}

static int on_chat_bytes(const char *bytes, size_t len, long status, void *user) {
    struct chat *c = user;
    if (status && (status < 200 || status > 299)) {
        size_t room = FAILURE_BODY_MAX - c->failure.len;
        if (room) mc_buf_append(&c->failure, bytes, len < room ? len : room);
        return 0;
    }
    return mc_sse_feed(&c->sse, bytes, len, on_chat_event, c) != 0;
}

static void describe_failure(long status, const mc_buf *body, char *out, size_t cap) {
    if (status == 401 || status == 403) {
        snprintf(out, cap, "Mistral rejected the key");
        return;
    }
    if (status == 429) {
        snprintf(out, cap, "Mistral is rate-limiting this key");
        return;
    }
    struct json_object *obj = body->len ? mc_json_parse((const char *)body->data, body->len) : NULL;
    const char *detail = mc_json_string(obj, "message");
    if (detail) snprintf(out, cap, "Mistral answered HTTP %ld: %s", status, detail);
    else snprintf(out, cap, "Mistral answered HTTP %ld", status);
    json_object_put(obj);
}

/* A reply for a turn that never started. */
static int refuse(int fd, const char *stage, const char *message) {
    struct json_object *msg = typed("error");
    json_object_object_add(msg, "stage", json_object_new_string(stage));
    json_object_object_add(msg, "message", json_object_new_string(message));
    int rc = mc_frame_write_json(fd, msg);
    json_object_put(msg);
    return rc;
}

int sewn_run_turn(sewn_service *svc, int fd, mc_frame_reader *reader, struct json_object *turn_start) {
    sewn_turn_request req;
    if (sewn_turn_request_parse(turn_start, &req) < 0) return refuse(fd, "request", "turn.start needs a request with messages");
    struct json_object *messages = sewn_turn_messages(&req);
    if (!messages) return refuse(fd, "request", "the request holds no user message");
    if (!svc->post_stream) {
        json_object_put(messages);
        return refuse(fd, "network", "sewnd was built without libcurl");
    }
    struct turn *t = calloc(1, sizeof *t);
    if (!t) {
        json_object_put(messages);
        return refuse(fd, "request", "out of memory");
    }
    int rc = sewn_key_store_get(&svc->keys, t->key, sizeof t->key);
    if (rc < 0) {
        free(t);
        json_object_put(messages);
        return refuse(fd, "key", rc == -ENOENT ? "no key is stored: add one in Settings \xE2\x80\xBA Mary" : "the stored key could not be read");
    }
    t->svc = svc;
    t->fd = fd;
    t->reader = reader;
    t->req = &req;
    t->started_ms = mc_now_ms();
    pthread_mutex_init(&t->write_lock, NULL);
    pthread_mutex_init(&t->lane_lock, NULL);
    pthread_cond_init(&t->lane_ready, NULL);
    pthread_t watcher, lane;
    pthread_create(&watcher, NULL, watch_client, t);
    pthread_create(&lane, NULL, speech_lane, t);

    struct json_object *phase = typed("phase");
    json_object_object_add(phase, "phase", json_object_new_string("grounded"));
    send_json(t, phase);

    struct json_object *body = sewn_chat_body(req.model, messages, req.max_tokens, req.temperature, req.top_p);
    size_t body_len = 0;
    const char *body_text = mc_json_compact(body, &body_len);
    struct chat c = { .t = t };
    mc_sse_init(&c.sse, SSE_LINE_MAX);
    sewn_chunker_init(&c.chunker);
    char message[256] = "";
    long status = 0;
    int sent = svc->post_stream(SEWN_MISTRAL_CHAT_PATH, t->key, body_text, body_len, on_chat_bytes, chat_should_stop, &c,
                                &status, message, sizeof message, svc->post_stream_user);
    bool http_ok = status == 0 || (status >= 200 && status <= 299);
    if (!atomic_load(&t->cancelled)) {
        if (sent >= 0 && http_ok && !c.finished) mc_sse_finish(&c.sse, on_chat_event, &c);
        if (sent < 0) {
            send_error(t, "grounded", message[0] ? message : "Mistral could not be reached");
        } else if (!http_ok) {
            char why[320];
            describe_failure(status, &c.failure, why, sizeof why);
            send_error(t, "grounded", why);
        }
        sewn_chunker_flush(&c.chunker, enqueue, t);
    }

    pthread_mutex_lock(&t->lane_lock);
    t->no_more = true;
    pthread_cond_broadcast(&t->lane_ready);
    pthread_mutex_unlock(&t->lane_lock);
    pthread_join(lane, NULL);
    bool cancelled = atomic_load(&t->cancelled);
    if (!cancelled) send_json(t, typed("turn.end"));
    atomic_store(&t->finished, true);
    pthread_join(watcher, NULL);

    mc_log(MC_LOG_INFO, "turn %s after %lld ms: %zu tokens, %zu sentences, first token %lld ms, first audio %lld ms",
           cancelled ? "cancelled" : "ended", (long long)(mc_now_ms() - t->started_ms), t->tokens, t->sentences,
           (long long)t->first_token_ms, (long long)t->first_audio_ms);

    mc_secure_zero(t->key, sizeof t->key);
    for (size_t i = 0; i < t->count; i++) free(t->queue[t->head + i]);
    free(t->queue);
    pthread_mutex_destroy(&t->write_lock);
    pthread_mutex_destroy(&t->lane_lock);
    pthread_cond_destroy(&t->lane_ready);
    free(t);
    mc_sse_free(&c.sse);
    sewn_chunker_free(&c.chunker);
    mc_buf_free(&c.failure);
    json_object_put(body);
    json_object_put(messages);
    return 0;
}
