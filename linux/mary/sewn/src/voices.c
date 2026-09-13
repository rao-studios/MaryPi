#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "sewn/voices.h"

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
#include "sewn/chunker.h"
#include "sewn/mistral.h"
#include "sewn/speech.h"

static struct json_object *typed(const char *type) {
    struct json_object *msg = json_object_new_object();
    json_object_object_add(msg, "type", json_object_new_string(type));
    return msg;
}

static int send_and_put(int fd, struct json_object *msg) {
    int rc = mc_frame_write_json(fd, msg);
    json_object_put(msg);
    return rc;
}

static int refuse(int fd, const char *stage, const char *message) {
    struct json_object *msg = typed("error");
    json_object_object_add(msg, "stage", json_object_new_string(stage));
    json_object_object_add(msg, "message", json_object_new_string(message));
    return send_and_put(fd, msg);
}

static int stored_key(sewn_service *svc, int fd, char *key, size_t cap) {
    int rc = sewn_key_store_get(&svc->keys, key, cap);
    if (rc < 0) refuse(fd, "key", rc == -ENOENT ? "no key is stored: add one in Settings \xE2\x80\xBA Mary" : "the stored key could not be read");
    return rc;
}

int sewn_voice_id_valid(const char *voice_id) {
    size_t n = voice_id ? strlen(voice_id) : 0;
    if (n == 0 || n > 63) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = voice_id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

/* MARK: - voices.list */

int sewn_voices_parse(const char *json, size_t len, struct json_object *into, int64_t *total) {
    if (total) *total = 0;
    struct json_object *page = mc_json_parse(json, len);
    struct json_object *items = mc_json_array(page, "items");
    if (!items) {
        json_object_put(page);
        return -EINVAL;
    }
    if (total) mc_json_int64(page, "total", total);
    size_t n = json_object_array_length(items);
    for (size_t i = 0; i < n; i++) {
        struct json_object *item = json_object_array_get_idx(items, i), *given = mc_json_array(item, "languages"), *owner;
        const char *id = mc_json_string(item, "id"), *slug = mc_json_string(item, "slug"), *name = mc_json_string(item, "name");
        const char *voice_id = slug && *slug ? slug : id, *gender = mc_json_string(item, "gender");
        if (!sewn_voice_id_valid(voice_id)) continue;
        struct json_object *voice = json_object_new_object(), *languages = json_object_new_array();
        json_object_object_add(voice, "voice_id", json_object_new_string(voice_id));
        if (id) json_object_object_add(voice, "id", json_object_new_string(id));
        json_object_object_add(voice, "name", json_object_new_string(name && *name ? name : voice_id));
        for (size_t k = 0; given && k < json_object_array_length(given); k++) {
            struct json_object *language = json_object_array_get_idx(given, k);
            if (json_object_is_type(language, json_type_string))
                json_object_array_add(languages, json_object_new_string(json_object_get_string(language)));
        }
        json_object_object_add(voice, "languages", languages);
        if (gender) json_object_object_add(voice, "gender", json_object_new_string(gender));
        bool custom = json_object_object_get_ex(item, "user_id", &owner) && owner && !json_object_is_type(owner, json_type_null);
        json_object_object_add(voice, "custom", json_object_new_boolean(custom));
        json_object_array_add(into, voice);
    }
    json_object_put(page);
    return 0;
}

int sewn_run_voices(sewn_service *svc, int fd) {
    if (!svc->get) return refuse(fd, "network", "sewnd was built without libcurl");
    char key[SEWN_KEY_MAX + 1];
    if (stored_key(svc, fd, key, sizeof key) < 0) return 0;
    struct json_object *voices = json_object_new_array();
    char message[256] = "";
    const char *stage = NULL;
    int64_t total = 0;
    for (int page = 0; page < SEWN_VOICES_PAGES_MAX && !stage; page++) {
        char path[128];
        snprintf(path, sizeof path, SEWN_VOICES_PATH "?type=all&limit=%d&offset=%d", SEWN_VOICES_PAGE, page * SEWN_VOICES_PAGE);
        mc_buf body = { 0 };
        long status = 0;
        int rc = svc->get(path, key, &body, SEWN_VOICES_BODY_MAX, &status, message, sizeof message, svc->get_user);
        if (rc < 0) {
            stage = "network";
        } else if (status == 401) {
            snprintf(message, sizeof message, "Mistral rejected the key");
            stage = "key";
        } else if (status < 200 || status > 299) {
            char reason[200];
            sewn_mistral_reason((const char *)body.data, body.len, reason, sizeof reason);
            if (reason[0]) snprintf(message, sizeof message, "Mistral answered HTTP %ld: %s", status, reason);
            else snprintf(message, sizeof message, "Mistral answered HTTP %ld", status);
            stage = "voices";
        } else if (sewn_voices_parse((const char *)body.data, body.len, voices, &total) < 0) {
            snprintf(message, sizeof message, "Mistral's list of voices could not be read");
            stage = "voices";
        }
        mc_buf_free(&body);
        if ((page + 1) * (int64_t)SEWN_VOICES_PAGE >= total) break;
    }
    mc_secure_zero(key, sizeof key);
    if (stage) {
        json_object_put(voices);
        mc_log(MC_LOG_WARNING, "the voices could not be listed: %s", message);
        return refuse(fd, stage, message[0] ? message : "Mistral could not be reached");
    }
    mc_log(MC_LOG_INFO, "listed %zu voices", json_object_array_length(voices));
    struct json_object *reply = typed("voices");
    json_object_object_add(reply, "voices", voices);
    return send_and_put(fd, reply);
}

/* MARK: - speak */

struct speaking {
    int fd;
    mc_frame_reader *reader;
    atomic_bool cancelled, finished;
    bool begun;
    size_t bytes;
};

static int on_client_frame(uint8_t kind, const unsigned char *bytes, size_t len, void *user) {
    struct speaking *s = user;
    if (kind != MC_FRAME_JSON) return 0;
    struct json_object *msg = mc_json_parse((const char *)bytes, len);
    const char *type = mc_json_type(msg);
    bool cancel = type && strcmp(type, "cancel") == 0;
    json_object_put(msg);
    if (!cancel) return 0;
    atomic_store(&s->cancelled, true);
    return 1;
}

/* A cancel frame, or the client going away, stops the speech, as a turn's does (turn.c). */
static void *watch_client(void *arg) {
    struct speaking *s = arg;
    if (mc_frame_reader_feed(s->reader, NULL, 0, on_client_frame, s) != 0) return NULL;
    while (!atomic_load(&s->finished) && !atomic_load(&s->cancelled)) {
        struct pollfd p = { .fd = s->fd, .events = POLLIN };
        int r = poll(&p, 1, 100);
        if (r < 0 && errno == EINTR) continue;
        if (r == 0) continue;
        int status = r < 0 ? -errno : mc_frame_reader_read_fd(s->reader, s->fd, on_client_frame, s);
        if (status == MC_IO_OK || status == -EAGAIN || status == -EWOULDBLOCK || status == -EINTR) continue;
        atomic_store(&s->cancelled, true);
        break;
    }
    return NULL;
}

static bool speech_should_stop(void *user) { return atomic_load(&((struct speaking *)user)->cancelled); }

static int on_pcm(const unsigned char *pcm, size_t len, void *user) {
    struct speaking *s = user;
    if (!s->begun) {
        struct json_object *begin = typed("audio.begin");
        json_object_object_add(begin, "sample_rate", json_object_new_int(SEWN_TTS_SAMPLE_RATE));
        json_object_object_add(begin, "channels", json_object_new_int(1));
        json_object_object_add(begin, "bits", json_object_new_int(32));
        json_object_object_add(begin, "encoding", json_object_new_string("f32le"));
        s->begun = true;
        if (send_and_put(s->fd, begin) < 0) {
            atomic_store(&s->cancelled, true);
            return 1;
        }
    }
    size_t max = MC_FRAME_MAX - MC_FRAME_MAX % 4;
    for (size_t off = 0; off < len; off += max) {
        size_t n = len - off < max ? len - off : max;
        if (mc_frame_write_fd(s->fd, MC_FRAME_PCM, pcm + off, n) < 0) {
            atomic_store(&s->cancelled, true);      /* the client has gone */
            return 1;
        }
    }
    s->bytes += len;
    return 0;
}

int sewn_run_speak(sewn_service *svc, int fd, mc_frame_reader *reader, struct json_object *request) {
    const char *voice_id = mc_json_string(request, "voice_id"), *text = mc_json_string(request, "text");
    const char *model = mc_json_string(request, "model");
    if (!sewn_voice_id_valid(voice_id)) return refuse(fd, "request", "speak needs a voice_id of letters, digits, _ and -");
    if (!text || !*text) return refuse(fd, "request", "speak needs some text");
    if (strlen(text) > SEWN_SPEAK_TEXT_MAX) return refuse(fd, "request", "speak takes at most 500 bytes of text");
    if (!svc->post_stream) return refuse(fd, "network", "sewnd was built without libcurl");
    char key[SEWN_KEY_MAX + 1];
    if (stored_key(svc, fd, key, sizeof key) < 0) return 0;
    char *clean = sewn_tts_sanitize(text);
    struct speaking s = { .fd = fd, .reader = reader };
    atomic_init(&s.cancelled, false);
    atomic_init(&s.finished, false);
    pthread_t watcher;
    bool watching = pthread_create(&watcher, NULL, watch_client, &s) == 0;
    sewn_speech speech = { .model = sewn_voice_id_valid(model) ? model : NULL, .voice_id = voice_id,
                           .text = clean && *clean ? clean : text };
    char why[256] = "";
    long status = 0;
    int64_t started = mc_now_ms();
    int rc = sewn_speak(svc, key, &speech, on_pcm, speech_should_stop, &s, &status, why, sizeof why);
    mc_secure_zero(key, sizeof key);
    free(clean);
    bool cancelled = atomic_load(&s.cancelled);
    if (!cancelled && rc < 0) {
        mc_log(MC_LOG_WARNING, "speech failed: %s", why);
        struct json_object *failed = typed("tts.failed");
        json_object_object_add(failed, "status", json_object_new_int64(status));
        json_object_object_add(failed, "message", json_object_new_string(why));
        send_and_put(fd, failed);
    }
    if (!cancelled) send_and_put(fd, typed("speak.end"));
    atomic_store(&s.finished, true);
    if (watching) pthread_join(watcher, NULL);
    mc_log(MC_LOG_INFO, "spoke in %s: %s after %lld ms, %zu bytes", voice_id,
           cancelled ? "cancelled" : rc < 0 ? "failed" : "ended", (long long)(mc_now_ms() - started), s.bytes);
    return 0;
}
