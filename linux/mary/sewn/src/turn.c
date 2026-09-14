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
#include "sewn/attribution.h"
#include "sewn/chunker.h"
#include "sewn/compact.h"
#include "sewn/embed.h"
#include "sewn/memory.h"
#include "sewn/mistral.h"
#include "sewn/outbound.h"
#include "sewn/speech.h"

/* Core/Sewn.swift handleChat. */
static const char BASE_RULES[] =
    "Keep responses under 6-7 sentences. Be specific and grounded. Never announce that you are an AI. "
    "Never output XML-like tags (such as <external>) in your response.";
/* Core/Sewn.swift handleChat: the citation-marker protocol under the context. */
static const char CITATION_PROTOCOL[] =
    "- The [n] tags label the sources in the context above; leave them there \xE2\x80\x94 do not copy [n] tags into your reply. "
    "Instead, when a sentence of yours draws on source [n], end that sentence with its doubled-bracket marker [[n]], placed "
    "after the closing punctuation (several in a row are fine, e.g. [[1]][[3]]). The markers are machine-read and stripped "
    "before the user sees your reply \xE2\x80\x94 never mention or explain them, and never use a number that does not appear in "
    "the context. This is a simple mechanical rule; apply it without deliberation.";

#define SSE_LINE_MAX (1u << 20)
#define FAILURE_BODY_MAX 2048

/* MARK: - The request */

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
    out->provider_known = sewn_provider_parse(mc_json_string(request, "provider"), &out->provider) == 0;
    if (!out->provider_known) out->provider = SEWN_PROVIDER_TINKER;
    out->model = sewn_provider_chat_model(out->provider_known ? out->provider : SEWN_PROVIDER_MISTRAL, mc_json_string(request, "model"));
    if (!out->model) out->model = SEWN_CHAT_MODEL;
    sewn_scope_parse(request, &out->scope);
    for (size_t i = 0; i < json_object_array_length(out->messages); i++) {
        struct json_object *m = json_object_array_get_idx(out->messages, i);
        const char *role = mc_json_string(m, "role"), *content = mc_json_string(m, "content");
        if (role && strcmp(role, "user") == 0) {
            out->user_messages++;
            if (content && *content) out->recent = content;
        }
    }
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

char *sewn_turn_system_prompt_with(const sewn_turn_request *req, const char *context) {
    mc_buf b = { 0 };
    int rc = 0;
    rc |= mc_buf_append_str(&b, "Your name is ");
    rc |= mc_buf_append_str(&b, req->persona_name ? req->persona_name : "Mary");
    rc |= mc_buf_append_str(&b, ".\n\n");
    if (req->persona_voice && *req->persona_voice) {
        rc |= mc_buf_append_str(&b, req->persona_voice);
        rc |= mc_buf_append_str(&b, " ");
    }
    rc |= mc_buf_append_str(&b, sewn_memory_instruction(!context || !*context));
    rc |= mc_buf_append_str(&b, "\n\n");
    if (req->instructions && *req->instructions) {
        rc |= mc_buf_append_str(&b, "--- CONVERSATIONAL INSTRUCTIONS ---\n");
        rc |= mc_buf_append_str(&b, req->instructions);
        rc |= mc_buf_append_str(&b, "\n\n");
    }
    rc |= mc_buf_append_str(&b, BASE_RULES);
    rc |= mc_buf_append_str(&b, "\n\n");
    if (context && *context) rc |= mc_buf_append_str(&b, context);
    if (rc) {
        mc_buf_free(&b);
        return NULL;
    }
    return (char *)b.data;
}

char *sewn_turn_system_prompt(const sewn_turn_request *req) { return sewn_turn_system_prompt_with(req, NULL); }

char *sewn_turn_context_block(const char *compacted) {
    char *guide = sewn_context_usage_guide(CITATION_PROTOCOL);
    mc_buf b = { 0 };
    mc_buf_append_str(&b, "--- CONTEXT ---\n");
    mc_buf_append_str(&b, compacted);
    mc_buf_append_str(&b, "\n\n");
    mc_buf_append_str(&b, guide);
    mc_buf_append_str(&b, "\n---");
    free(guide);
    return (char *)b.data;
}

static struct json_object *role_content(const char *role, const char *content) {
    struct json_object *m = json_object_new_object();
    json_object_object_add(m, "role", json_object_new_string(role));
    json_object_object_add(m, "content", json_object_new_string(content));
    return m;
}

struct json_object *sewn_turn_history(const sewn_turn_request *req) {
    struct json_object *out = json_object_new_array();
    size_t n = json_object_array_length(req->messages);
    long last_user = -1;
    for (size_t i = 0; i < n; i++) {
        struct json_object *m = json_object_array_get_idx(req->messages, i);
        const char *role = mc_json_string(m, "role"), *content = mc_json_string(m, "content");
        if (role && content && *content && strcmp(role, "user") == 0) last_user = (long)i;
    }
    for (size_t i = 0; i < n; i++) {
        if ((long)i == last_user) continue;
        struct json_object *m = json_object_array_get_idx(req->messages, i);
        const char *role = mc_json_string(m, "role"), *content = mc_json_string(m, "content");
        if (!role || !content || !*content || (strcmp(role, "user") && strcmp(role, "assistant"))) continue;
        json_object_array_add(out, role_content(role, content));
    }
    return out;
}

struct json_object *sewn_turn_messages_with(const sewn_turn_request *req, const char *system, int history_turns) {
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
    if (last_user < 0 || !system) {
        free(usable);
        return NULL;
    }
    struct json_object *out = json_object_new_array();
    long first = last_user > history_turns ? last_user - history_turns : 0;
    for (long k = first; k <= last_user; k++) {
        struct json_object *m = json_object_array_get_idx(req->messages, usable[k]);
        json_object_array_add(out, role_content(mc_json_string(m, "role"), mc_json_string(m, "content")));
    }
    json_object_array_add(out, role_content("system", system));
    free(usable);
    return out;
}

struct json_object *sewn_turn_messages(const sewn_turn_request *req) {
    char *system = sewn_turn_system_prompt(req);
    struct json_object *out = system ? sewn_turn_messages_with(req, system, SEWN_HISTORY_TURNS) : NULL;
    free(system);
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
    /* The reply as Mistral wrote it (markers and all) and the filter that strips them. */
    mc_buf raw;
    sewn_marker_filter markers;
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

static int lane_pcm(const unsigned char *pcm, size_t len, void *user) { return send_pcm(user, pcm, len) < 0; }

static bool lane_should_stop(void *user) {
    struct turn *t = user;
    return atomic_load(&t->cancelled);
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
        sewn_speech speech = { .model = t->req->tts_model, .voice_id = t->req->voice_id, .text = sentence };
        char why[256] = "";
        long status = 0;
        int rc = sewn_speak(t->svc, t->key, &speech, lane_pcm, lane_should_stop, t, &status, why, sizeof why);
        free(sentence);
        if (rc < 0) {
            /* Sewn's TTS lane ends at its first failure; the words keep coming, and the client hears why. */
            if (rc != -ECANCELED && !atomic_load(&t->cancelled)) {
                mc_log(MC_LOG_WARNING, "speech failed: %s", why);
                struct json_object *failed = typed("tts.failed");
                json_object_object_add(failed, "status", json_object_new_int64(status));
                json_object_object_add(failed, "message", json_object_new_string(why));
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
            mc_buf_append(&c->t->raw, e.content, n);
            char *visible = sewn_marker_filter_feed(&c->t->markers, e.content, n);
            size_t vn = strlen(visible);
            if (vn && send_token(c->t, visible, vn) < 0) stop = 1;
            else if (vn) sewn_chunker_feed(&c->chunker, visible, vn, enqueue, c->t);
            free(visible);
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

/* MARK: Retrieval and the context */

static const char *const DEFAULT_LANES[] = { "personal" };

struct grounding {
    sewn_retrieval retrieval;
    sewn_compact_result compact;
    char *context;              /* the --- CONTEXT --- block, or NULL */
    bool searched;
};

/* Sewn's search → compact, in the lanes the request names (personal when it names none). Never fails the turn: without the Thread, the turn runs as one
 * with no context. */
static void ground(sewn_service *svc, struct turn *t, const sewn_turn_request *req, const char *owner, struct grounding *g) {
    memset(g, 0, sizeof *g);
    if (!svc->retrieve || !req->recent || !*req->recent) return;
    sewn_scope scope = req->scope;
    scope.owner_id = owner;
    if (!scope.n_lanes) {
        for (size_t i = 0; i < sizeof DEFAULT_LANES / sizeof DEFAULT_LANES[0]; i++) scope.lanes[scope.n_lanes++] = DEFAULT_LANES[i];
    }
    char message[256] = "";
    int rc = svc->retrieve(&scope, req->recent, SEWN_RETRIEVE_TOP_K, &g->retrieval, message, sizeof message, svc->retrieve_user);
    if (rc < 0) {
        mc_log(MC_LOG_WARNING, "retrieval skipped: %s", message[0] ? message : strerror(-rc));
        return;
    }
    g->searched = true;
    struct json_object *frame = typed("retrieval");
    struct json_object *lanes = json_object_new_array(), *parts = json_object_new_array();
    for (size_t i = 0; i < scope.n_lanes; i++) json_object_array_add(lanes, json_object_new_string(scope.lanes[i]));
    for (size_t i = 0; i < g->retrieval.n; i++) {
        const sewn_partition *p = &g->retrieval.partitions[i];
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "document_id", json_object_new_string(p->document_id));
        json_object_object_add(o, "partition_id", json_object_new_string(p->partition_id));
        json_object_object_add(o, "name", json_object_new_string(p->name));
        json_object_object_add(o, "family", json_object_new_string(p->family));
        json_object_object_add(o, "lane", json_object_new_string(p->lane));
        json_object_object_add(o, "group_id", json_object_new_string(p->group_id));
        json_object_object_add(o, "owner_id", json_object_new_string(p->owner_id));
        json_object_object_add(o, "score", json_object_new_double(p->score));
        json_object_array_add(parts, o);
    }
    json_object_object_add(frame, "lanes", lanes);
    json_object_object_add(frame, "partitions", parts);
    json_object_object_add(frame, "ms", json_object_new_int64(g->retrieval.ms));
    send_json(t, frame);
    if (!g->retrieval.n) return;
    struct json_object *history = sewn_turn_history(req);
    int64_t started = mc_now_ms();
    rc = sewn_compact(svc, t->key, req->provider, history, g->retrieval.partitions, g->retrieval.n, owner, req->scope.request_id, &g->compact,
                      message, sizeof message);
    json_object_put(history);
    if (rc < 0) {
        mc_log(MC_LOG_WARNING, "compaction failed, using the context verbatim: %s", message);
        sewn_compact_result_free(&g->compact);
        g->compact.text = sewn_compact_verbatim(g->retrieval.partitions, g->retrieval.n, owner, &g->compact.source_index);
        g->compact.used_verbatim = true;
    }
    mc_log(MC_LOG_INFO, "[timing] search %lld ms (%zu partitions), compact %lld ms (verbatim: %s)", (long long)g->retrieval.ms, g->retrieval.n,
           (long long)(mc_now_ms() - started), g->compact.used_verbatim ? "yes" : "no");
    g->context = sewn_turn_context_block(g->compact.text);
}

static void grounding_free(struct grounding *g) {
    sewn_retrieval_free(&g->retrieval);
    sewn_compact_result_free(&g->compact);
    free(g->context);
}

/* Gita on the finished reply: the contribution with spans, and turn.end. */
static void finish_turn(struct turn *t, const sewn_turn_request *req, struct grounding *g) {
    sewn_contribution contribution;
    sewn_contribution_build(g->retrieval.partitions, g->retrieval.n, "", &contribution);
    char *visible = NULL;
    const char *raw = t->raw.data ? (const char *)t->raw.data : "";
    sewn_annotate(raw, &contribution, g->retrieval.partitions, g->retrieval.n, g->compact.citations, g->compact.source_index, &visible);
    struct json_object *end = typed("turn.end");
    json_object_object_add(end, "text", json_object_new_string(visible ? visible : ""));
    json_object_object_add(end, "contribution", sewn_contribution_json(&contribution));
    struct json_object *retrieved = json_object_new_array();
    for (size_t i = 0; i < g->retrieval.n; i++) {
        const sewn_partition *p = &g->retrieval.partitions[i];
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "document_id", json_object_new_string(p->document_id));
        json_object_object_add(o, "group_id", json_object_new_string(p->group_id));
        json_object_object_add(o, "name", json_object_new_string(p->name));
        json_object_object_add(o, "family", json_object_new_string(p->family));
        json_object_object_add(o, "lane", json_object_new_string(p->lane));
        json_object_object_add(o, "score", json_object_new_double(p->score));
        json_object_array_add(retrieved, o);
    }
    json_object_object_add(end, "retrieved", retrieved);
    json_object_object_add(end, "provider", json_object_new_string(sewn_provider_name(req->provider)));
    json_object_object_add(end, "model", json_object_new_string(req->model));
    send_json(t, end);
    free(visible);
    sewn_contribution_free(&contribution);
}

/* Auto-memory, after the reply: the count trigger, else the topic-change trigger
 * (the cosine between the last two user messages' embeddings). */
static void remember(sewn_service *svc, struct turn *t, const sewn_turn_request *req, const char *owner) {
    if (!svc->deposit || !req->recent) return;
    bool due = sewn_memory_count_due(req->user_messages);
    const char *reason = "messageCount";
    if (!due && req->user_messages >= 2) {
        const char *previous = NULL;
        for (size_t i = json_object_array_length(req->messages); i > 0; i--) {
            struct json_object *m = json_object_array_get_idx(req->messages, i - 1);
            const char *role = mc_json_string(m, "role"), *content = mc_json_string(m, "content");
            if (role && content && *content && strcmp(role, "user") == 0 && content != req->recent) {
                previous = content;
                break;
            }
        }
        if (previous) {
            const char *texts[2] = { previous, req->recent };
            float *vectors = NULL;
            size_t dim = 0;
            long status = 0;
            char message[256] = "";
            if (sewn_embed(svc, t->key, texts, 2, "memory", req->scope.request_id, &vectors, &dim, &status, message, sizeof message) == 0) {
                float cosine = sewn_cosine(vectors, vectors + dim, dim);
                due = cosine < SEWN_MEMORY_TOPIC_COSINE;
                reason = "topicChange";
                free(vectors);
            }
        }
    }
    if (!due) return;
    struct json_object *history = sewn_turn_history(req);
    char document_id[256] = "", message[256] = "";
    int rc = sewn_memorize(svc, t->key, req->provider, owner, history, req->recent, req->scope.request_id, document_id, sizeof document_id,
                           message, sizeof message);
    json_object_put(history);
    if (rc == 0) mc_log(MC_LOG_INFO, "memory %s stored for %s (%s)", document_id, owner, reason);
    else if (rc != -ENOENT) mc_log(MC_LOG_WARNING, "auto-memory failed for %s: %s", owner, message);
}

int sewn_run_turn(sewn_service *svc, int fd, mc_frame_reader *reader, struct json_object *turn_start, const char *owner) {
    sewn_turn_request req;
    if (sewn_turn_request_parse(turn_start, &req) < 0) return refuse(fd, "request", "turn.start needs a request with messages");
    if (!req.provider_known || !sewn_provider_available(req.provider)) return refuse(fd, "engine", SEWN_ENGINE_UNAVAILABLE);
    if (!req.recent) return refuse(fd, "request", "the request holds no user message");
    if (!svc->post_stream) return refuse(fd, "network", "sewnd was built without libcurl");
    struct turn *t = calloc(1, sizeof *t);
    if (!t) return refuse(fd, "request", "out of memory");
    int rc = sewn_key_store_get(&svc->keys, t->key, sizeof t->key);
    if (rc < 0) {
        free(t);
        return refuse(fd, "key", rc == -ENOENT ? "no key is stored: add one in Settings \xE2\x80\xBA Mary" : "the stored key could not be read");
    }
    t->svc = svc;
    t->fd = fd;
    t->reader = reader;
    t->req = &req;
    t->started_ms = mc_now_ms();
    sewn_marker_filter_init(&t->markers);
    pthread_mutex_init(&t->write_lock, NULL);
    pthread_mutex_init(&t->lane_lock, NULL);
    pthread_cond_init(&t->lane_ready, NULL);
    pthread_t watcher, lane;
    pthread_create(&watcher, NULL, watch_client, t);
    pthread_create(&lane, NULL, speech_lane, t);

    struct json_object *phase = typed("phase");
    json_object_object_add(phase, "phase", json_object_new_string("grounded"));
    send_json(t, phase);

    struct grounding g;
    ground(svc, t, &req, owner ? owner : "", &g);
    char *system = sewn_turn_system_prompt_with(&req, g.context);
    /* Verbatim context carries no conversation summary, so the history rides as real turns;
     * the briefing already holds it. */
    struct json_object *messages = system ? sewn_turn_messages_with(&req, system, g.context && !g.compact.used_verbatim ? 0 : SEWN_HISTORY_TURNS) : NULL;
    free(system);
    struct json_object *body = messages ? sewn_chat_body(req.model, messages, req.max_tokens, req.temperature, req.top_p) : NULL;
    size_t body_len = 0;
    const char *body_text = body ? mc_json_compact(body, &body_len) : "";
    struct chat c = { .t = t };
    mc_sse_init(&c.sse, SSE_LINE_MAX);
    sewn_chunker_init(&c.chunker);
    char message[256] = "";
    long status = 0;
    sewn_outbound outbound = { req.provider, "chat", req.scope.request_id };
    int sent = body ? sewn_post(svc, &outbound, SEWN_MISTRAL_CHAT_PATH, t->key, body_text, body_len, on_chat_bytes, chat_should_stop, &c,
                                &status, message, sizeof message) : -ENOMEM;
    bool http_ok = status == 0 || (status >= 200 && status <= 299);
    if (!atomic_load(&t->cancelled)) {
        if (sent >= 0 && http_ok && !c.finished) mc_sse_finish(&c.sse, on_chat_event, &c);
        char *tail = sewn_marker_filter_finish(&t->markers);
        if (*tail) {
            send_token(t, tail, strlen(tail));
            sewn_chunker_feed(&c.chunker, tail, strlen(tail), enqueue, t);
        }
        free(tail);
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
    if (!cancelled) finish_turn(t, &req, &g);
    atomic_store(&t->finished, true);
    pthread_join(watcher, NULL);

    mc_log(MC_LOG_INFO, "turn %s after %lld ms: %zu tokens, %zu sentences, first token %lld ms, first audio %lld ms, %zu retrieved",
           cancelled ? "cancelled" : "ended", (long long)(mc_now_ms() - t->started_ms), t->tokens, t->sentences,
           (long long)t->first_token_ms, (long long)t->first_audio_ms, g.retrieval.n);

    if (!cancelled && sent >= 0 && http_ok) remember(svc, t, &req, owner ? owner : "");

    mc_secure_zero(t->key, sizeof t->key);
    for (size_t i = 0; i < t->count; i++) free(t->queue[t->head + i]);
    free(t->queue);
    mc_buf_free(&t->raw);
    pthread_mutex_destroy(&t->write_lock);
    pthread_mutex_destroy(&t->lane_lock);
    pthread_cond_destroy(&t->lane_ready);
    free(t);
    grounding_free(&g);
    mc_sse_free(&c.sse);
    sewn_chunker_free(&c.chunker);
    mc_buf_free(&c.failure);
    if (body) json_object_put(body);
    if (messages) json_object_put(messages);
    return 0;
}
