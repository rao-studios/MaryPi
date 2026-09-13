#include "runtime/ears.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/io.h"
#include "common/json.h"
#include "common/log.h"
#include "runtime/transcriber.h"
#include "voice/ring.h"
#include "voice/vad.h"
#include "voice/wake.h"

enum command { CMD_NONE, CMD_LISTEN, CMD_FOLLOW_UP, CMD_STANDBY, CMD_MUTE };
enum mode { MODE_OFF, MODE_STANDBY, MODE_LISTENING, MODE_WAITING };

/* One transcription: what its reader thread's callbacks need. */
struct session {
    struct mr_ears *ears;
    int id;
    bool woke, follow_up;
    pthread_mutex_t lock;
    char partial[1024];
};

struct mr_ears {
    mr_ears_config config;
    mr_queue *events;
    mv_kws *kws;
    mv_ring ring;
    int wake_pipe[2];
    pthread_t thread;
    atomic_bool stop, wake;
    atomic_int command;

    /* The thread's own. */
    enum mode mode;
    int16_t preroll[MR_EARS_PREROLL_FRAMES * MR_EARS_FRAME];
    size_t preroll_next;
    bool preroll_full;
    mr_transcriber *transcriber;
    struct session *session;
    int sessions;
    mv_vad vad;
    bool speech_seen;
    int64_t opened_ms, speech_ms;
    unsigned frames;
};

static void post(struct mr_ears *e, struct json_object *event) { mr_queue_push(e->events, event); }

static struct json_object *event(const char *type, int session) {
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "type", json_object_new_string(type));
    if (session > 0) json_object_object_add(o, "session", json_object_new_int(session));
    return o;
}

static void post_state(struct mr_ears *e, const char *state) {
    struct json_object *o = event("ears.state", e->session ? e->session->id : 0);
    json_object_object_add(o, "state", json_object_new_string(state));
    post(e, o);
}

static void on_delta(const char *text, void *user) {
    struct session *s = user;
    pthread_mutex_lock(&s->lock);
    snprintf(s->partial, sizeof s->partial, "%s", text);
    pthread_mutex_unlock(&s->lock);
    struct json_object *o = event("ears.partial", s->id);
    json_object_object_add(o, "text", json_object_new_string(text));
    post(s->ears, o);
}

static void on_done(const char *text, void *user) {
    struct session *s = user;
    struct json_object *o = event("ears.heard", s->id);
    json_object_object_add(o, "text", json_object_new_string(text));
    json_object_object_add(o, "woke", json_object_new_boolean(s->woke));
    json_object_object_add(o, "follow_up", json_object_new_boolean(s->follow_up));
    post(s->ears, o);
}

static void on_error(const char *stage, const char *message, void *user) {
    struct session *s = user;
    struct json_object *o = event("ears.error", s->id);
    json_object_object_add(o, "stage", json_object_new_string(stage));
    json_object_object_add(o, "message", json_object_new_string(message));
    post(s->ears, o);
}

static const mr_transcript_events transcript_events = { .delta = on_delta, .done = on_done, .error = on_error };

static void close_session(struct mr_ears *e, bool cancel) {
    if (e->transcriber) {
        if (cancel) mr_transcriber_cancel(e->transcriber);
        mr_transcriber_free(e->transcriber);
        e->transcriber = NULL;
    }
    if (e->session) {
        pthread_mutex_destroy(&e->session->lock);
        free(e->session);
        e->session = NULL;
    }
}

static void open_session(struct mr_ears *e, bool woke, bool follow_up) {
    struct session *s = calloc(1, sizeof *s);
    if (!s) return;
    s->ears = e;
    s->id = ++e->sessions;
    s->woke = woke;
    s->follow_up = follow_up;
    pthread_mutex_init(&s->lock, NULL);
    e->session = s;
    int error = 0;
    e->transcriber = mr_transcriber_open(e->config.sewn_socket, MR_EARS_RATE, &transcript_events, s, &error);
    if (!e->transcriber) {
        on_error("sewnd", error == -ENOENT || error == -ECONNREFUSED ? "sewnd is not running" : strerror(-error), s);
        close_session(e, false);
        e->mode = MODE_STANDBY;
        return;
    }
    if (woke && e->preroll_full) {
        size_t n = sizeof e->preroll / sizeof *e->preroll;
        mr_transcriber_send(e->transcriber, e->preroll + e->preroll_next, n - e->preroll_next);
        mr_transcriber_send(e->transcriber, e->preroll, e->preroll_next);
    } else if (woke) {
        mr_transcriber_send(e->transcriber, e->preroll, e->preroll_next);
    }
    mv_vad_reset(&e->vad);
    e->speech_seen = false;
    e->opened_ms = mc_now_ms();
    e->mode = MODE_LISTENING;
    post_state(e, "listening");
}

static void keep_preroll(struct mr_ears *e, const int16_t *frame) {
    memcpy(e->preroll + e->preroll_next, frame, MR_EARS_FRAME * sizeof *frame);
    e->preroll_next += MR_EARS_FRAME;
    if (e->preroll_next == sizeof e->preroll / sizeof *e->preroll) {
        e->preroll_next = 0;
        e->preroll_full = true;
    }
}

static void listen_frame(struct mr_ears *e, const int16_t *frame) {
    if (mr_transcriber_send(e->transcriber, frame, MR_EARS_FRAME) < 0) return;    /* the reader reports it */
    float rms = mv_rms_s16(frame, MR_EARS_FRAME);
    if (++e->frames % 3 == 0) {
        struct json_object *o = event("ears.level", 0);
        json_object_object_add(o, "rms", json_object_new_double(rms));
        post(e, o);
    }
    char partial[1024];
    pthread_mutex_lock(&e->session->lock);
    snprintf(partial, sizeof partial, "%s", e->session->partial);
    pthread_mutex_unlock(&e->session->lock);
    e->vad.hangover_extension = mv_endpoint_extra_silence(partial);
    mv_vad_verdict v = mv_vad_process(&e->vad, rms, (double)MR_EARS_FRAME / MR_EARS_RATE);
    bool too_long = e->speech_seen && mc_now_ms() - e->speech_ms > e->config.max_utterance_ms;
    if (v.kind == MV_VAD_SPEECH_START) {
        if (!e->speech_seen) e->speech_ms = mc_now_ms();
        e->speech_seen = true;
        post_state(e, "hearing");
    } else if (v.kind == MV_VAD_SPEECH_END || too_long) {
        mr_transcriber_end(e->transcriber);
        e->mode = MODE_WAITING;
        post_state(e, "transcribing");
    }
}

static void handle_frame(struct mr_ears *e, const int16_t *frame) {
    keep_preroll(e, frame);
    if (e->mode == MODE_STANDBY && e->kws && atomic_load(&e->wake)) {
        char keyword[64];
        if (mv_kws_feed(e->kws, frame, MR_EARS_FRAME, keyword, sizeof keyword) == 1) {
            struct json_object *o = event("ears.wake", 0);
            json_object_object_add(o, "keyword", json_object_new_string(keyword));
            post(e, o);
            open_session(e, true, false);
        }
    } else if (e->mode == MODE_LISTENING) {
        listen_frame(e, frame);
    }
}

static void handle_command(struct mr_ears *e, int command) {
    switch (command) {
    case CMD_LISTEN:
    case CMD_FOLLOW_UP:
        if (e->mode == MODE_LISTENING || e->mode == MODE_WAITING) break;
        close_session(e, true);
        open_session(e, false, command == CMD_FOLLOW_UP);
        break;
    case CMD_STANDBY:
    case CMD_MUTE:
        close_session(e, true);
        e->mode = command == CMD_MUTE ? MODE_OFF : MODE_STANDBY;
        if (e->kws) mv_kws_reset(e->kws);
        break;
    }
}

static void check_session(struct mr_ears *e) {
    if (!e->transcriber) return;
    if (e->mode == MODE_LISTENING && !e->speech_seen && mc_now_ms() - e->opened_ms > e->config.listen_timeout_ms) {
        int id = e->session->id;
        close_session(e, true);
        e->mode = MODE_STANDBY;
        post(e, event("ears.timeout", id));
    } else if (mr_transcriber_closed(e->transcriber)) {
        /* heard (or failed) is posted: pause until maryd says what comes next. */
        bool failed = e->mode == MODE_LISTENING;
        close_session(e, false);
        e->mode = failed ? MODE_STANDBY : MODE_OFF;
    }
}

static void *run(void *arg) {
    struct mr_ears *e = arg;
    float samples[MR_EARS_FRAME];
    int16_t frame[MR_EARS_FRAME];
    while (!atomic_load(&e->stop)) {
        struct pollfd p = { .fd = e->wake_pipe[0], .events = POLLIN };
        poll(&p, 1, 100);
        char bytes[256];
        while (read(e->wake_pipe[0], bytes, sizeof bytes) > 0) {
        }
        int command = atomic_exchange(&e->command, CMD_NONE);
        if (command != CMD_NONE) handle_command(e, command);
        while (mv_ring_available(&e->ring) >= MR_EARS_FRAME) {
            mv_ring_read(&e->ring, samples, MR_EARS_FRAME);
            for (int i = 0; i < MR_EARS_FRAME; i++) frame[i] = (int16_t)samples[i];
            handle_frame(e, frame);
            command = atomic_exchange(&e->command, CMD_NONE);
            if (command != CMD_NONE) handle_command(e, command);
        }
        check_session(e);
    }
    close_session(e, true);
    return NULL;
}

mr_ears *mr_ears_new(const mr_ears_config *config, mr_queue *events, int *error) {
    struct mr_ears *e = calloc(1, sizeof *e);
    int rc = e ? 0 : -ENOMEM;
    if (rc == 0) rc = mv_ring_init(&e->ring, 4 * MR_EARS_RATE);
    if (rc == 0 && pipe(e->wake_pipe) < 0) rc = -errno;
    if (rc < 0) {
        if (e) mv_ring_free(&e->ring);
        free(e);
        if (error) *error = rc;
        return NULL;
    }
    for (int i = 0; i < 2; i++) {
        mc_set_nonblocking(e->wake_pipe[i], true);
        mc_set_cloexec(e->wake_pipe[i]);
    }
    e->config = *config;
    if (e->config.listen_timeout_ms <= 0) e->config.listen_timeout_ms = 6000;
    if (e->config.max_utterance_ms <= 0) e->config.max_utterance_ms = 30000;
    e->events = events;
    e->mode = MODE_STANDBY;
    atomic_init(&e->stop, false);
    atomic_init(&e->wake, config->wake);
    atomic_init(&e->command, CMD_NONE);
    mv_vad_config vad = mv_vad_config_default();
    mv_vad_init(&e->vad, &vad);
    if (config->kws) {
        int kws_error = 0;
        e->kws = mv_kws_open(config->kws, &kws_error);
        if (!e->kws) mc_log(MC_LOG_NOTICE, "no wake word: %s", kws_error == -ENOSYS ? "built without sherpa-onnx" : strerror(-kws_error));
    }
    if (pthread_create(&e->thread, NULL, run, e) != 0) {
        mv_kws_close(e->kws);
        mv_ring_free(&e->ring);
        close(e->wake_pipe[0]);
        close(e->wake_pipe[1]);
        free(e);
        if (error) *error = -EAGAIN;
        return NULL;
    }
    return e;
}

void mr_ears_free(mr_ears *e) {
    if (!e) return;
    atomic_store(&e->stop, true);
    ssize_t ignored = write(e->wake_pipe[1], "", 1);
    (void)ignored;
    pthread_join(e->thread, NULL);
    mv_kws_close(e->kws);
    mv_ring_free(&e->ring);
    close(e->wake_pipe[0]);
    close(e->wake_pipe[1]);
    free(e);
}

void mr_ears_hear(mr_ears *e, const int16_t *samples, size_t count) {
    float chunk[MR_EARS_FRAME];
    while (count) {
        size_t n = count < MR_EARS_FRAME ? count : MR_EARS_FRAME;
        for (size_t i = 0; i < n; i++) chunk[i] = samples[i];
        mv_ring_write(&e->ring, chunk, n);      /* a full ring drops audio rather than wait */
        samples += n;
        count -= n;
    }
    ssize_t ignored = write(e->wake_pipe[1], "", 1);
    (void)ignored;
}

static void command(struct mr_ears *e, enum command c) {
    atomic_store(&e->command, c);
    ssize_t ignored = write(e->wake_pipe[1], "", 1);
    (void)ignored;
}

void mr_ears_listen(mr_ears *e, bool follow_up) { command(e, follow_up ? CMD_FOLLOW_UP : CMD_LISTEN); }
void mr_ears_standby(mr_ears *e) { command(e, CMD_STANDBY); }
void mr_ears_mute(mr_ears *e) { command(e, CMD_MUTE); }
void mr_ears_set_wake(mr_ears *e, bool on) { atomic_store(&e->wake, on); }
bool mr_ears_can_wake(const mr_ears *e) { return e->kws && atomic_load(&((struct mr_ears *)e)->wake); }

static bool blank(const char *s) {
    while (s && *s && isspace((unsigned char)*s)) s++;
    return !s || !*s;
}

mr_heard_kind mr_heard(const char *text, bool woke, char *request, size_t cap) {
    if (cap) request[0] = 0;
    if (blank(text)) return MR_HEARD_NOTHING;
    if (mv_is_stop_listening(text)) return MR_HEARD_STOP;
    if (woke) {
        switch (mv_wake_in(text, request, cap)) {
        case MV_WAKE_BARE: return MR_HEARD_NOTHING;
        case MV_WAKE_REQUEST: return blank(request) ? MR_HEARD_NOTHING : MR_HEARD_REQUEST;
        case MV_WAKE_NONE: break;
        }
    }
    const char *start = text;
    while (isspace((unsigned char)*start)) start++;
    size_t n = strlen(start);
    while (n && isspace((unsigned char)start[n - 1])) n--;
    if (n >= cap) n = cap ? cap - 1 : 0;
    if (cap) {
        memcpy(request, start, n);
        request[n] = 0;
    }
    return MR_HEARD_REQUEST;
}
