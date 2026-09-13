#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "sewn/transcribe.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/io.h"
#include "common/json.h"
#include "common/log.h"
#include "common/secure.h"
#include "sewn/mistral.h"

struct session {
    sewn_service *svc;
    int fd;
    sewn_ws *ws;
    pthread_mutex_t write_lock;
    /* Set by Voxtral's thread, under lock. */
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool ready, done, failed, closed, error_sent;
    char closed_reason[160];
    /* The client's side, on this thread only. */
    bool ended, cancelled, ws_broken;
    size_t audio_bytes, appends;
};

static struct json_object *typed(const char *type) {
    struct json_object *msg = json_object_new_object();
    json_object_object_add(msg, "type", json_object_new_string(type));
    return msg;
}

static void send_json(struct session *s, struct json_object *msg) {
    pthread_mutex_lock(&s->write_lock);
    mc_frame_write_json(s->fd, msg);
    pthread_mutex_unlock(&s->write_lock);
    json_object_put(msg);
}

static void send_error_once(struct session *s, const char *stage, const char *message) {
    pthread_mutex_lock(&s->lock);
    bool already = s->error_sent;
    s->error_sent = true;
    pthread_mutex_unlock(&s->lock);
    if (already) return;
    struct json_object *msg = typed("error");
    json_object_object_add(msg, "stage", json_object_new_string(stage));
    json_object_object_add(msg, "message", json_object_new_string(message));
    send_json(s, msg);
}

/* MARK: - Voxtral's side */

static void on_voxtral(const char *text, size_t len, void *user) {
    struct session *s = user;
    sewn_stt_event e;
    if (sewn_stt_event_parse(text, len, &e) < 0) return;
    switch (e.kind) {
    case SEWN_STT_SESSION: {
        pthread_mutex_lock(&s->lock);
        bool first = !s->ready;
        s->ready = true;
        pthread_mutex_unlock(&s->lock);
        if (first) send_json(s, typed("transcribe.ready"));
        break;
    }
    case SEWN_STT_TEXT_DELTA:
        if (e.text && *e.text) {
            struct json_object *msg = typed("transcript.delta");
            json_object_object_add(msg, "text", json_object_new_string(e.text));
            send_json(s, msg);
        }
        break;
    case SEWN_STT_DONE: {
        struct json_object *msg = typed("transcript.done");
        json_object_object_add(msg, "text", json_object_new_string(e.text ? e.text : ""));
        send_json(s, msg);
        pthread_mutex_lock(&s->lock);
        s->done = true;
        pthread_cond_broadcast(&s->changed);
        pthread_mutex_unlock(&s->lock);
        break;
    }
    case SEWN_STT_ERROR:
        send_error_once(s, "transcribe", e.text && *e.text ? e.text : "Voxtral reported an error");
        pthread_mutex_lock(&s->lock);
        s->failed = true;
        pthread_cond_broadcast(&s->changed);
        pthread_mutex_unlock(&s->lock);
        break;
    default:
        break;
    }
    sewn_stt_event_release(&e);
}

static void on_voxtral_closed(const char *reason, void *user) {
    struct session *s = user;
    pthread_mutex_lock(&s->lock);
    s->closed = true;
    snprintf(s->closed_reason, sizeof s->closed_reason, "%s", reason && *reason ? reason : "Voxtral closed the session");
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->lock);
}

static int send_to_voxtral(struct session *s, struct json_object *msg) {
    if (!msg) {
        s->ws_broken = true;
        return -ENOMEM;
    }
    size_t len = 0;
    const char *text = mc_json_compact(msg, &len);
    int rc = text ? s->svc->ws->send(s->ws, text, len) : -ENOMEM;
    json_object_put(msg);
    if (rc < 0) s->ws_broken = true;
    return rc;
}

/* MARK: - The client's side */

static int on_client_frame(uint8_t kind, const unsigned char *bytes, size_t len, void *user) {
    struct session *s = user;
    if (kind == MC_FRAME_PCM) {
        for (size_t off = 0; off < len; off += SEWN_STT_MAX_APPEND) {
            size_t n = len - off < SEWN_STT_MAX_APPEND ? len - off : SEWN_STT_MAX_APPEND;
            if (send_to_voxtral(s, sewn_stt_append(bytes + off, n)) < 0) return 1;
            s->appends++;
        }
        s->audio_bytes += len;
        return 0;
    }
    if (kind != MC_FRAME_JSON) return 0;
    struct json_object *msg = mc_json_parse((const char *)bytes, len);
    const char *type = mc_json_type(msg);
    int stop = 0;
    if (type && strcmp(type, "transcribe.flush") == 0) {
        stop = send_to_voxtral(s, sewn_stt_flush()) < 0;
    } else if (type && strcmp(type, "transcribe.end") == 0) {
        s->ended = true;
        send_to_voxtral(s, sewn_stt_end());
        stop = 1;
    } else if (type && strcmp(type, "cancel") == 0) {
        s->cancelled = true;
        stop = 1;
    }
    json_object_put(msg);
    return stop;
}

static bool voxtral_over(struct session *s) {
    pthread_mutex_lock(&s->lock);
    bool over = s->done || s->failed || s->closed;
    pthread_mutex_unlock(&s->lock);
    return over;
}

static int voxtral_rate(int64_t rate) {
    switch (rate) {
    case 8000: case 16000: case 22050: case 44100: case 48000: return (int)rate;
    default: return SEWN_STT_SAMPLE_RATE;
    }
}

static int refuse(int fd, const char *stage, const char *message) {
    struct json_object *msg = typed("error");
    json_object_object_add(msg, "stage", json_object_new_string(stage));
    json_object_object_add(msg, "message", json_object_new_string(message));
    int rc = mc_frame_write_json(fd, msg);
    json_object_put(msg);
    return rc;
}

static void wait_for_the_transcript(struct session *s, int wait_ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += wait_ms / 1000;
    deadline.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&s->lock);
    while (!s->done && !s->failed && !s->closed)
        if (pthread_cond_timedwait(&s->changed, &s->lock, &deadline) == ETIMEDOUT) break;
    pthread_mutex_unlock(&s->lock);
}

int sewn_run_transcribe(sewn_service *svc, int fd, mc_frame_reader *reader, struct json_object *start) {
    int64_t value = 0;
    int sample_rate = mc_json_int64(start, "sample_rate", &value) ? voxtral_rate(value) : SEWN_STT_SAMPLE_RATE;
    int delay_ms = mc_json_int64(start, "delay_ms", &value) && value > 0 && value <= 5000 ? (int)value : 0;
    if (!svc->ws) return refuse(fd, "network", "sewnd was built without libwebsockets");

    char key[SEWN_KEY_MAX + 1];
    int rc = sewn_key_store_get(&svc->keys, key, sizeof key);
    if (rc < 0) return refuse(fd, "key", rc == -ENOENT ? "no key is stored: add one in Settings \xE2\x80\xBA Mary" : "the stored key could not be read");
    struct session *s = calloc(1, sizeof *s);
    if (!s) {
        mc_secure_zero(key, sizeof key);
        return refuse(fd, "request", "out of memory");
    }
    s->svc = svc;
    s->fd = fd;
    pthread_mutex_init(&s->write_lock, NULL);
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->changed, NULL);

    char path[192], message[256] = "";
    snprintf(path, sizeof path, "%s?model=%s", SEWN_MISTRAL_REALTIME_PATH, SEWN_STT_MODEL);
    int64_t started = mc_now_ms();
    s->ws = svc->ws->open(path, key, on_voxtral, on_voxtral_closed, s, message, sizeof message, svc->ws_user);
    mc_secure_zero(key, sizeof key);
    if (!s->ws) {
        refuse(fd, "network", message[0] ? message : "Voxtral could not be reached");
    } else {
        send_to_voxtral(s, sewn_stt_session_update(sample_rate, delay_ms));

        /* Audio the client sent together with transcribe.start is already buffered. */
        bool stopped = mc_frame_reader_feed(reader, NULL, 0, on_client_frame, s) != 0;
        while (!stopped && !s->ws_broken && !voxtral_over(s)) {
            if (mc_now_ms() - started > SEWN_TRANSCRIBE_MAX_MS) {
                s->ended = true;
                send_to_voxtral(s, sewn_stt_end());
                break;
            }
            struct pollfd p = { .fd = fd, .events = POLLIN };
            int r = poll(&p, 1, 100);
            if (r < 0 && errno == EINTR) continue;
            if (r == 0) continue;
            int status = r < 0 ? -errno : mc_frame_reader_read_fd(reader, fd, on_client_frame, s);
            if (status == MC_IO_OK || status == -EAGAIN || status == -EWOULDBLOCK || status == -EINTR) continue;
            if (status != MC_IO_STOPPED) s->cancelled = true;   /* the client hung up */
            stopped = true;
        }

        if (s->ended && !s->cancelled && !s->ws_broken)
            wait_for_the_transcript(s, svc->transcribe_wait_ms > 0 ? svc->transcribe_wait_ms : SEWN_TRANSCRIBE_DONE_WAIT_MS);
        if (!s->cancelled) {
            pthread_mutex_lock(&s->lock);
            bool finished = s->done || s->failed, closed = s->closed;
            char reason[160];
            snprintf(reason, sizeof reason, "%s", s->closed_reason);
            pthread_mutex_unlock(&s->lock);
            if (!finished) {
                const char *why = s->ws_broken ? "the connection to Voxtral broke"
                                  : closed      ? reason
                                  : s->ended    ? "Voxtral did not finish the transcript in time"
                                                : "the transcription ended early";
                send_error_once(s, "transcribe", why);
            }
        }
        svc->ws->close(s->ws);
        mc_log(MC_LOG_INFO, "transcription %s: %.1f s of audio in %zu appends",
               s->cancelled ? "cancelled" : s->done ? "done" : "failed",
               (double)s->audio_bytes / (2.0 * sample_rate), s->appends);
    }
    pthread_mutex_destroy(&s->write_lock);
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->changed);
    free(s);
    return 0;
}
