#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "sewn/server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/base64.h"
#include "common/buf.h"
#include "common/frame.h"
#include "common/io.h"
#include "common/json.h"
#include "common/log.h"
#include "common/peer.h"
#include "common/secure.h"
#include "sewn/compact.h"
#include "sewn/complete.h"
#include "sewn/embed.h"
#include "sewn/http.h"
#include "sewn/memory.h"
#include "sewn/mistral.h"
#include "sewn/outbound.h"
#include "sewn/transcribe.h"
#include "sewn/turn.h"
#include "sewn/voices.h"
#include "sewn/ws.h"

#define SEWN_EXTRACT_INPUT_MAX 3000     /* MistralGraphExtractionProvider.maxInputChars */

void sewn_service_init(sewn_service *svc, const char *state_dir) {
    memset(svc, 0, sizeof *svc);
    sewn_key_store_init(&svc->keys, state_dir);
    svc->admin_group = SEWN_ADMIN_GROUP;
    svc->in_group = sewn_uid_in_group;
#ifdef HAVE_CURL
    svc->verify = sewn_mistral_verify;
    svc->get = sewn_http_get;
    svc->post_stream = sewn_http_post_stream;
#endif
#ifdef HAVE_LWS
    svc->ws = &sewn_lws_ops;
#endif
    svc->retrieve = sewn_thread_retrieve;
    svc->deposit = sewn_thread_deposit;
    sewn_calls_init(&svc->calls, state_dir);
}

void sewn_service_free(sewn_service *svc) { sewn_calls_free(&svc->calls); }

int sewn_send_error(int fd, const char *stage, const char *message, long status) {
    struct json_object *reply = json_object_new_object();
    json_object_object_add(reply, "type", json_object_new_string("error"));
    json_object_object_add(reply, "stage", json_object_new_string(stage));
    json_object_object_add(reply, "message", json_object_new_string(message));
    if (status) json_object_object_add(reply, "status", json_object_new_int64(status));
    int rc = mc_frame_write_json(fd, reply);
    json_object_put(reply);
    return rc;
}

static int send_error(int fd, const char *stage, const char *message) { return sewn_send_error(fd, stage, message, 0); }

int sewn_stored_key(sewn_service *svc, int fd, char *key, size_t cap) {
    int rc = sewn_key_store_get(&svc->keys, key, cap);
    if (rc == -ENOENT) send_error(fd, "key", "no key is stored: add one in Settings \xE2\x80\xBA Mary");
    else if (rc < 0) {
        mc_log(MC_LOG_ERROR, "the stored key could not be read: %s", strerror(-rc));
        send_error(fd, "key", "the stored key could not be read");
    }
    return rc;
}

/* ok < 0: no verdict to report. */
static int send_status(int fd, const sewn_service *svc, int ok, const char *message) {
    struct json_object *reply = json_object_new_object();
    json_object_object_add(reply, "type", json_object_new_string("key.status"));
    json_object_object_add(reply, "present", json_object_new_boolean(sewn_key_store_present(&svc->keys)));
    int64_t at = sewn_key_store_verified_at(&svc->keys);
    json_object_object_add(reply, "verified_at", at > 0 ? json_object_new_int64(at) : NULL);
    if (ok >= 0) json_object_object_add(reply, "ok", json_object_new_boolean(ok));
    if (message) json_object_object_add(reply, "message", json_object_new_string(message));
    int rc = mc_frame_write_json(fd, reply);
    json_object_put(reply);
    return rc;
}

static int key_set(sewn_service *svc, int fd, struct json_object *request) {
    struct json_object *value;
    if (!json_object_object_get_ex(request, "key", &value) || !json_object_is_type(value, json_type_string))
        return send_error(fd, "key", "key.set needs a key");
    char *key = (char *)json_object_get_string(value);
    size_t stored = (size_t)json_object_get_string_len(value), len = stored;
    while (len && (key[len - 1] == '\n' || key[len - 1] == '\r')) len--;
    int rc = sewn_key_store_set(&svc->keys, key, len);
    mc_secure_zero(key, stored);
    if (rc == -EINVAL) return send_error(fd, "key", "that is not a Mistral API key");
    if (rc < 0) {
        mc_log(MC_LOG_ERROR, "the key could not be stored in %s: %s", svc->keys.dir, strerror(-rc));
        return send_error(fd, "key", "the key could not be stored");
    }
    mc_log(MC_LOG_NOTICE, "a new key was stored");
    return send_status(fd, svc, -1, NULL);
}

static int key_verify(sewn_service *svc, int fd) {
    char key[SEWN_KEY_MAX + 1], message[256] = "";
    int rc = sewn_key_store_get(&svc->keys, key, sizeof key);
    if (rc == -ENOENT) return send_error(fd, "key", "no key is stored");
    if (rc < 0) {
        mc_log(MC_LOG_ERROR, "the stored key could not be read: %s", strerror(-rc));
        return send_error(fd, "key", "the stored key could not be read");
    }
    if (!svc->verify) {
        mc_secure_zero(key, sizeof key);
        return send_error(fd, "network", "sewnd was built without libcurl");
    }
    int64_t started = mc_now_ms();
    int verdict = svc->verify(key, message, sizeof message, svc->verify_user);
    mc_secure_zero(key, sizeof key);
    sewn_outbound o = { SEWN_PROVIDER_MISTRAL, "verify", NULL };
    sewn_record_call(svc, &o, "/v1/models", verdict > 0 ? 200 : verdict == 0 ? 401 : 0, mc_now_ms() - started, 0, 0,
                     verdict < 0 ? "failed" : "ok");
    if (verdict < 0) return send_error(fd, "network", message[0] ? message : "Mistral could not be reached");
    if (verdict > 0) sewn_key_store_mark_verified(&svc->keys, mc_wall_ms());
    mc_log(MC_LOG_NOTICE, "Mistral %s the key", verdict > 0 ? "accepted" : "refused");
    return send_status(fd, svc, verdict > 0, message[0] ? message : NULL);
}

/* MARK: - The ops threadd and maryd call */

/* A provider from the request, or an engine error sent. */
static int provider_of(int fd, struct json_object *request, sewn_provider *out) {
    if (sewn_provider_parse(mc_json_string(request, "provider"), out) < 0 || !sewn_provider_available(*out)) {
        send_error(fd, "engine", SEWN_ENGINE_UNAVAILABLE);
        return -ENOTSUP;
    }
    return 0;
}

static const char **strings_of(struct json_object *arr, size_t *n) {
    *n = 0;
    if (!arr || !json_object_is_type(arr, json_type_array) || !json_object_array_length(arr)) return NULL;
    const char **v = calloc(json_object_array_length(arr), sizeof *v);
    for (size_t i = 0; v && i < json_object_array_length(arr); i++) {
        const char *s = json_object_get_string(json_object_array_get_idx(arr, i));
        v[(*n)++] = s ? s : "";
    }
    return v;
}

static int model_error(int fd, int rc, long status, const char *message) {
    const char *stage = rc == -ENOSYS ? "network" : status == 401 || status == 403 ? "key" : "network";
    return sewn_send_error(fd, stage, message[0] ? message : "Mistral could not be reached", status);
}

static int op_embed(sewn_service *svc, int fd, struct json_object *request) {
    size_t n = 0;
    const char **texts = strings_of(mc_json_array(request, "texts"), &n);
    if (!n) return send_error(fd, "request", "embed needs texts");
    char key[SEWN_KEY_MAX + 1];
    if (sewn_stored_key(svc, fd, key, sizeof key) < 0) {
        free(texts);
        return 0;
    }
    float *vectors = NULL;
    size_t dim = 0;
    long status = 0;
    char message[256] = "";
    const char *purpose = mc_json_string(request, "purpose");
    int rc = sewn_embed(svc, key, texts, n, purpose && *purpose ? purpose : "embed", mc_json_string(request, "request_id"), &vectors, &dim,
                        &status, message, sizeof message);
    mc_secure_zero(key, sizeof key);
    free(texts);
    if (rc < 0) return model_error(fd, rc, status, message);
    struct json_object *reply = json_object_new_object(), *arr = json_object_new_array();
    json_object_object_add(reply, "type", json_object_new_string("embed.result"));
    json_object_object_add(reply, "model", json_object_new_string(SEWN_EMBED_MODEL));
    json_object_object_add(reply, "dim", json_object_new_int64((int64_t)dim));
    for (size_t i = 0; i < n; i++) {
        size_t len = 0;
        char *b64 = mc_base64_encode_alloc((const unsigned char *)(vectors + i * dim), dim * sizeof(float), &len);
        json_object_array_add(arr, json_object_new_string(b64 ? b64 : ""));
        free(b64);
    }
    json_object_object_add(reply, "vectors_b64", arr);
    free(vectors);
    rc = mc_frame_write_json(fd, reply);
    json_object_put(reply);
    return rc;
}

static int op_extract(sewn_service *svc, int fd, struct json_object *request) {
    size_t n = 0;
    const char **texts = strings_of(mc_json_array(request, "texts"), &n);
    const char *prompt = mc_json_string(request, "prompt");
    if (!n || !prompt || !*prompt) {
        free(texts);
        return send_error(fd, "request", "graph.extract needs texts and a prompt");
    }
    sewn_provider provider;
    if (provider_of(fd, request, &provider) < 0) {
        free(texts);
        return 0;
    }
    char key[SEWN_KEY_MAX + 1];
    if (sewn_stored_key(svc, fd, key, sizeof key) < 0) {
        free(texts);
        return 0;
    }
    /* The texts joined, cut to the provider's input budget on a character boundary. */
    mc_buf input = { 0 };
    for (size_t i = 0; i < n; i++) {
        if (i) mc_buf_append_str(&input, "\n\n");
        mc_buf_append_str(&input, texts[i]);
    }
    free(texts);
    if (!input.data) mc_buf_append_str(&input, "");
    size_t chars = 0, cut = input.len;
    for (size_t i = 0; i < input.len; i++) {
        if (((unsigned char)input.data[i] & 0xC0) == 0x80) continue;
        if (chars == SEWN_EXTRACT_INPUT_MAX) {
            cut = i;
            break;
        }
        chars++;
    }
    input.data[cut] = 0;
    char *text = NULL, message[256] = "";
    int rc = sewn_complete_text(svc, key, provider, prompt, (const char *)input.data, 800, 0, "extract", mc_json_string(request, "request_id"),
                                &text, message, sizeof message);
    mc_secure_zero(key, sizeof key);
    mc_buf_free(&input);
    if (rc < 0) return model_error(fd, rc, 0, message);
    struct json_object *reply = json_object_new_object();
    json_object_object_add(reply, "type", json_object_new_string("graph.extract.result"));
    json_object_object_add(reply, "json", json_object_new_string(text));
    free(text);
    rc = mc_frame_write_json(fd, reply);
    json_object_put(reply);
    return rc;
}

static int op_summarize(sewn_service *svc, int fd, struct json_object *request) {
    struct json_object *messages = mc_json_array(request, "messages");
    const char *recent = mc_json_string(request, "recent");
    if (!messages && !recent) return send_error(fd, "request", "summarize needs messages");
    sewn_provider provider;
    if (provider_of(fd, request, &provider) < 0) return 0;
    char key[SEWN_KEY_MAX + 1];
    if (sewn_stored_key(svc, fd, key, sizeof key) < 0) return 0;
    char *transcript = sewn_memory_transcript(messages, recent);
    char *summary = NULL, message[256] = "";
    int rc = *transcript ? sewn_complete_text(svc, key, provider, sewn_memory_prompt(), transcript, SEWN_MEMORY_MAX_TOKENS, SEWN_DEFAULT_TEMPERATURE,
                                               "summarize", mc_json_string(request, "request_id"), &summary, message, sizeof message)
                         : -EINVAL;
    mc_secure_zero(key, sizeof key);
    free(transcript);
    if (rc == -EINVAL) return send_error(fd, "request", "nothing to summarize");
    if (rc < 0) return model_error(fd, rc, 0, message);
    char **lines = NULL;
    size_t n = sewn_memory_lines(summary, &lines);
    struct json_object *reply = json_object_new_object(), *arr = json_object_new_array();
    json_object_object_add(reply, "type", json_object_new_string("summarize.result"));
    json_object_object_add(reply, "text", json_object_new_string(summary));
    for (size_t i = 0; i < n; i++) json_object_array_add(arr, json_object_new_string(lines[i]));
    json_object_object_add(reply, "lines", arr);
    sewn_memory_lines_free(lines, n);
    free(summary);
    rc = mc_frame_write_json(fd, reply);
    json_object_put(reply);
    return rc;
}

static const char *const SKILL_LANES[] = { "behavioral", "application" };

/* /v1/skills/complete: roles kept, the tool roster offered, tool_calls answered. A
 * `scope` retrieves Mary's own records (behavioral + application unless it names
 * lanes) as a background tier under the instructions. */
static int op_complete(sewn_service *svc, int fd, struct json_object *request, const char *owner) {
    struct json_object *messages = mc_json_array(request, "messages");
    if (!messages || !json_object_array_length(messages)) return send_error(fd, "request", "complete needs messages");
    sewn_provider provider;
    if (provider_of(fd, request, &provider) < 0) return 0;
    char key[SEWN_KEY_MAX + 1];
    if (sewn_stored_key(svc, fd, key, sizeof key) < 0) return 0;
    mc_buf system = { 0 };
    const char *instructions = mc_json_string(request, "instructions");
    if (instructions && *instructions) mc_buf_append_str(&system, instructions);
    struct json_object *retrieved = json_object_new_array();
    sewn_retrieval retrieval = { 0 };
    if (svc->retrieve && mc_json_object(request, "scope")) {
        sewn_scope scope;
        struct json_object *wrap = json_object_new_object();
        json_object_object_add(wrap, "sewn", json_object_get(mc_json_object(request, "scope")));
        sewn_scope_parse(wrap, &scope);
        scope.owner_id = owner;
        if (!scope.n_lanes) for (size_t i = 0; i < 2; i++) scope.lanes[scope.n_lanes++] = SKILL_LANES[i];
        const char *query = NULL;
        for (size_t i = json_object_array_length(messages); i > 0 && !query; i--) {
            struct json_object *m = json_object_array_get_idx(messages, i - 1);
            const char *role = mc_json_string(m, "role");
            if (role && strcmp(role, "user") == 0) query = mc_json_string(m, "content");
        }
        char message[256] = "";
        if (query && *query && svc->retrieve(&scope, query, SEWN_RETRIEVE_TOP_K, &retrieval, message, sizeof message, svc->retrieve_user) == 0 && retrieval.n) {
            char *block = sewn_compact_verbatim(retrieval.partitions, retrieval.n, owner, NULL);
            if (system.len) mc_buf_append_str(&system, "\n\n");
            mc_buf_append_str(&system, "--- BACKGROUND ---\nMary's own records, retrieved for this request: what the applications offer and what "
                                       "she did before. Background only \xE2\x80\x94 never the subject of the reply, never recited.\n");
            mc_buf_append_str(&system, block);
            mc_buf_append_str(&system, "\n---");
            free(block);
            for (size_t i = 0; i < retrieval.n; i++) {
                const sewn_partition *p = &retrieval.partitions[i];
                struct json_object *o = json_object_new_object();
                json_object_object_add(o, "document_id", json_object_new_string(p->document_id));
                json_object_object_add(o, "name", json_object_new_string(p->name));
                json_object_object_add(o, "family", json_object_new_string(p->family));
                json_object_object_add(o, "lane", json_object_new_string(p->lane));
                json_object_object_add(o, "score", json_object_new_double(p->score));
                json_object_array_add(retrieved, o);
            }
        }
        json_object_put(wrap);
    }
    int64_t max_tokens = 0;
    mc_json_int64(request, "max_tokens", &max_tokens);
    if (max_tokens <= 0) max_tokens = SEWN_SKILLS_MAX_TOKENS_DEFAULT;
    if (max_tokens < SEWN_SKILLS_MAX_TOKENS_MIN) max_tokens = SEWN_SKILLS_MAX_TOKENS_MIN;
    if (max_tokens > SEWN_SKILLS_MAX_TOKENS_MAX) max_tokens = SEWN_SKILLS_MAX_TOKENS_MAX;
    double temperature = 0;
    mc_json_double(request, "temperature", &temperature);
    const char *purpose = mc_json_string(request, "purpose");
    sewn_complete_request req = { .provider = provider, .system = system.len ? (const char *)system.data : NULL, .messages = messages,
                                  .tools = mc_json_array(request, "tools"), .max_tokens = (int)max_tokens, .temperature = temperature,
                                  .purpose = purpose && *purpose ? purpose : "skills", .request_id = mc_json_string(request, "request_id") };
    sewn_completion out;
    char message[256] = "";
    int rc = sewn_complete(svc, key, &req, &out, message, sizeof message);
    mc_secure_zero(key, sizeof key);
    mc_buf_free(&system);
    sewn_retrieval_free(&retrieval);
    if (rc < 0) {
        json_object_put(retrieved);
        return model_error(fd, rc, out.status, message);
    }
    if (!out.tool_calls) sewn_completion_recover_tool_calls(&out.text, &out.tool_calls);
    struct json_object *reply = json_object_new_object();
    json_object_object_add(reply, "type", json_object_new_string("complete.result"));
    json_object_object_add(reply, "text", json_object_new_string(out.text));
    json_object_object_add(reply, "tool_calls", out.tool_calls ? json_object_get(out.tool_calls) : json_object_new_array());
    json_object_object_add(reply, "retrieved", retrieved);
    json_object_object_add(reply, "provider", json_object_new_string(sewn_provider_name(provider)));
    sewn_completion_free(&out);
    rc = mc_frame_write_json(fd, reply);
    json_object_put(reply);
    return rc;
}

static int op_calls(sewn_service *svc, int fd, struct json_object *request, bool stats) {
    struct json_object *reply = json_object_new_object();
    json_object_object_add(reply, "type", json_object_new_string(stats ? "calls.stats.result" : "calls.list.result"));
    if (stats) {
        json_object_object_add(reply, "stats", sewn_calls_stats(&svc->calls));
    } else {
        int64_t limit = 0;
        mc_json_int64(request, "limit", &limit);
        json_object_object_add(reply, "calls", sewn_calls_list(&svc->calls, (int)limit));
    }
    int rc = mc_frame_write_json(fd, reply);
    json_object_put(reply);
    return rc;
}

struct first_frame {
    mc_buf *payload;
    int kind;
};

static int take_first(uint8_t kind, const unsigned char *bytes, size_t len, void *user) {
    struct first_frame *first = user;
    first->kind = mc_buf_append(first->payload, bytes, len) < 0 ? -ENOMEM : kind;
    return 1;
}

/* An operation name fit for a log line: short, printable, or a stand-in. */
static const char *loggable(const char *type) {
    size_t n = strlen(type);
    if (n == 0 || n > 32) return "(unnamed)";
    for (size_t i = 0; i < n; i++) if (type[i] < 0x21 || type[i] > 0x7E) return "(unnamed)";
    return type;
}

static int dispatch(sewn_service *svc, int fd, const sewn_peer *peer, mc_frame_reader *reader, struct json_object *request) {
    const char *type = mc_json_type(request);
    if (!type) return send_error(fd, "request", "the first frame must be a JSON message with a type");
    if (!sewn_peer_may(peer, type, svc->admin_group, svc->in_group)) {
        mc_log(MC_LOG_WARNING, "refused %s from uid %u", loggable(type), (unsigned)peer->uid);
        return send_error(fd, "auth", "this user may not do that");
    }
    mc_log(MC_LOG_INFO, "%s from uid %u", loggable(type), (unsigned)peer->uid);
    char owner[64] = "";
    mc_user_name(peer->uid, owner, sizeof owner);
    if (strcmp(type, "key.status") == 0) return send_status(fd, svc, -1, NULL);
    if (strcmp(type, "key.set") == 0) return key_set(svc, fd, request);
    if (strcmp(type, "key.verify") == 0) return key_verify(svc, fd);
    if (strcmp(type, "turn.start") == 0) return sewn_run_turn(svc, fd, reader, request, owner);
    if (strcmp(type, "complete") == 0) return op_complete(svc, fd, request, owner);
    if (strcmp(type, "embed") == 0) return op_embed(svc, fd, request);
    if (strcmp(type, "graph.extract") == 0) return op_extract(svc, fd, request);
    if (strcmp(type, "summarize") == 0) return op_summarize(svc, fd, request);
    if (strcmp(type, "calls.list") == 0) return op_calls(svc, fd, request, false);
    if (strcmp(type, "calls.stats") == 0) return op_calls(svc, fd, request, true);
    if (strcmp(type, "transcribe.start") == 0) return sewn_run_transcribe(svc, fd, reader, request);
    if (strcmp(type, "voices.list") == 0) return sewn_run_voices(svc, fd);
    if (strcmp(type, "speak") == 0) return sewn_run_speak(svc, fd, reader, request);
    return send_error(fd, "request", "unknown operation");
}

int sewn_serve_connection(sewn_service *svc, int fd, const sewn_peer *peer) {
    mc_frame_reader reader;
    mc_frame_reader_init(&reader, 0, true);
    mc_buf payload = { 0 };
    struct first_frame first = { &payload, 0 };
    int status, rc;
    while ((status = mc_frame_reader_read_fd(&reader, fd, take_first, &first)) == MC_IO_OK) {}
    if (status != MC_IO_STOPPED) {
        rc = status < 0 ? status : -ECONNRESET;
    } else if (first.kind < 0) {
        rc = first.kind;
    } else if (first.kind != MC_FRAME_JSON) {
        rc = send_error(fd, "request", "the first frame must be JSON");
    } else {
        struct json_object *request = mc_json_parse_secret((const char *)payload.data, payload.len);
        mc_secure_zero(payload.data, payload.len);
        rc = dispatch(svc, fd, peer, &reader, request);
        json_object_put(request);
    }
    mc_buf_free_secure(&payload);
    mc_frame_reader_free(&reader);
    return rc;
}

int sewn_listen(const char *path, int mode) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    size_t n = strlen(path);
    if (n >= sizeof addr.sun_path) return -ENAMETOOLONG;
    memcpy(addr.sun_path, path, n + 1);
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) return -EEXIST;
        unlink(path);
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -errno;
    mc_set_cloexec(fd);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || chmod(path, (mode_t)mode) < 0 || listen(fd, 16) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}
