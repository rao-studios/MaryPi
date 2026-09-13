#include "runtime/daemon.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "brain/clock.h"
#include "brain/history.h"
#include "brain/prompt.h"
#include "brain/request.h"
#include "common/io.h"
#include "common/json.h"
#include "common/log.h"
#include "common/peer.h"
#include "common/secure.h"
#include "computer-use/invoke.h"
#include "mary-thread/client.h"
#include "runtime/desktop.h"
#include "runtime/ears.h"
#include "runtime/queue.h"
#include "runtime/turn.h"
#include "sewn/client.h"
#include "sewn/key.h"
#include "skills/registry.h"
#include "voice/audio.h"
#include "voice/state.h"

struct mr_daemon {
    mr_config config;
    char desktop_path[256], sewn_path[256], thread_path[256], owner[64];
    mr_queue queue;
    mr_desktop desktop;
    mr_ears *ears;
    mv_audio *audio;
    mcu_pipes *pipes;
    sk_registry skills;
    struct json_object *skills_message;
    mb_history history;
    atomic_bool stop;
    atomic_int workers;
    mv_state state;
    bool key_present;
    int ears_session;

    struct {
        mr_turn *worker;
        int gen;
        bool voice, failed, audio_started, draining;
        char *question;
        mc_buf reply;
        int64_t started_wall, quiet_since;
    } turn;
    atomic_bool turn_stopping;      /* the audio callback gives up waiting for room */
    atomic_int turn_gen;
};

mr_config mr_config_default(void) {
    return (mr_config){ .audio = true, .wake = true, .listen_timeout_ms = 6000, .echo_tail_ms = 300,
                        .skill_timeout_ms = 10000 };
}

static struct json_object *typed(const char *type) {
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "type", json_object_new_string(type));
    return o;
}

static void broadcast(mr_daemon *d, struct json_object *msg) {
    mr_desktop_broadcast(&d->desktop, msg);
    json_object_put(msg);
}

static void send_to(mr_daemon *d, mr_client *c, struct json_object *msg) {
    mr_desktop_send(&d->desktop, c, msg);
    json_object_put(msg);
}

static struct json_object *error_message(const char *stage, const char *message) {
    struct json_object *o = typed("error");
    json_object_object_add(o, "stage", json_object_new_string(stage));
    json_object_object_add(o, "message", json_object_new_string(message));
    return o;
}

static void set_state(mr_daemon *d, mv_state state) {
    if (d->state == state) return;
    d->state = state;
    struct json_object *o = typed("state");
    json_object_object_add(o, "state", json_object_new_string(mv_state_name(state)));
    broadcast(d, o);
}

/* The microphone is open only while it could matter: spotting the name, or a session. */
static void microphone(mr_daemon *d, bool session) {
    if (d->audio) mv_audio_capture(d->audio, session || mr_ears_can_wake(d->ears));
}

static void standby(mr_daemon *d) {
    mr_ears_standby(d->ears);
    microphone(d, false);
}

static void listen_now(mr_daemon *d, bool follow_up) {
    microphone(d, true);
    mr_ears_listen(d->ears, follow_up);
}

/* ---- workers: short jobs on detached threads that post their outcome ---- */

static void spawn(mr_daemon *d, void *(*fn)(void *), void *job) {
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    atomic_fetch_add(&d->workers, 1);
    if (pthread_create(&thread, &attr, fn, job) != 0) {
        atomic_fetch_sub(&d->workers, 1);
        mc_log(MC_LOG_ERROR, "could not start a worker thread");
        free(job);
    }
    pthread_attr_destroy(&attr);
}

struct key_job {
    mr_daemon *d;
    char type[16];
    bool quiet;             /* a background status check: say nothing when sewnd is away */
    char *key;
    size_t key_len;
};

static void *key_worker(void *arg) {
    struct key_job *job = arg;
    mr_daemon *d = job->d;
    struct json_object *reply = NULL, *event = typed("key.reply");
    int fd = sewn_connect(d->sewn_path), rc = fd;
    if (fd >= 0) {
        struct timeval patience = { .tv_sec = 20 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof patience);
        if (job->key) {
            rc = sewn_call_key_set(fd, job->key, job->key_len, &reply);
        } else {
            struct json_object *request = typed(job->type);
            rc = sewn_call(fd, request, &reply);
            json_object_put(request);
        }
        close(fd);
    }
    if (job->key) {
        mc_secure_zero(job->key, job->key_len);
        free(job->key);
    }
    json_object_object_add(event, "quiet", json_object_new_boolean(job->quiet));
    if (rc < 0) json_object_object_add(event, "failure", json_object_new_string(
        rc == -ENOENT || rc == -ECONNREFUSED ? "sewnd is not running" : strerror(-rc)));
    else json_object_object_add(event, "reply", reply);
    mr_queue_push(&d->queue, event);
    atomic_fetch_sub(&d->workers, 1);
    free(job);
    return NULL;
}

static void key_job(mr_daemon *d, const char *type, bool quiet, const char *key, size_t key_len) {
    struct key_job *job = calloc(1, sizeof *job);
    if (!job) return;
    job->d = d;
    snprintf(job->type, sizeof job->type, "%s", type);
    job->quiet = quiet;
    if (key && (job->key = malloc(key_len + 1))) {
        memcpy(job->key, key, key_len);
        job->key[key_len] = 0;
        job->key_len = key_len;
    }
    spawn(d, key_worker, job);
}

struct deposit_job {
    mr_daemon *d;
    mt_turn turn;
    char *question, *reply;
    char owner[64];
};

static void *deposit_worker(void *arg) {
    struct deposit_job *job = arg;
    char id[64];
    int status = 0;
    int rc = mt_deposit_turn(job->d->thread_path, &job->turn, MT_TIMEOUT_MS, id, sizeof id, &status);
    if (rc == 0) mc_log(MC_LOG_DEBUG, "deposited %s in Thread", id);
    else if (rc == -EPROTO) mc_log(MC_LOG_WARNING, "threadd refused the turn (status %d)", status);
    else mc_log(MC_LOG_WARNING, "could not reach threadd: %s", strerror(-rc));
    atomic_fetch_sub(&job->d->workers, 1);
    free(job->question);
    free(job->reply);
    free(job);
    return NULL;
}

static void deposit(mr_daemon *d, bool cancelled) {
    struct deposit_job *job = calloc(1, sizeof *job);
    if (!job || !(job->question = strdup(d->turn.question)) ||
        !(job->reply = strdup(d->turn.reply.data ? (char *)d->turn.reply.data : ""))) {
        if (job) free(job->question);
        free(job);
        return;
    }
    job->d = d;
    snprintf(job->owner, sizeof job->owner, "%s", d->owner);
    job->turn = (mt_turn){ .owner_id = job->owner, .user_text = job->question, .reply = job->reply,
                           .source = d->turn.voice ? "voice" : "typed", .started_ms = d->turn.started_wall,
                           .ended_ms = mc_wall_ms(), .cancelled = cancelled };
    spawn(d, deposit_worker, job);
}

/* ---- the turn ---- */

struct turn_ctx {
    mr_daemon *d;
    int gen;
    bool heard_audio;
};

static void post_turn(struct turn_ctx *t, struct json_object *event) {
    json_object_object_add(event, "gen", json_object_new_int(t->gen));
    mr_queue_push(&t->d->queue, event);
}

static void on_token(const char *text, void *user) {
    struct json_object *o = typed("turn.token");
    json_object_object_add(o, "text", json_object_new_string(text));
    post_turn(user, o);
}

static void on_audio(const float *samples, size_t count, void *user) {
    struct turn_ctx *t = user;
    if (!t->heard_audio) {
        t->heard_audio = true;
        post_turn(t, typed("turn.audio"));
    }
    mr_daemon *d = t->d;
    while (d->audio && count && !atomic_load(&d->turn_stopping) && atomic_load(&d->turn_gen) == t->gen) {
        size_t n = mv_audio_play(d->audio, samples, count);
        samples += n;
        count -= n;
        if (!n) usleep(10000);      /* the ring holds a minute; wait for the speaker */
    }
}

static void on_turn_error(const char *stage, const char *message, void *user) {
    struct json_object *o = typed("turn.error");
    json_object_object_add(o, "stage", json_object_new_string(stage));
    json_object_object_add(o, "message", json_object_new_string(message));
    post_turn(user, o);
}

static void on_turn_end(bool completed, void *user) {
    struct json_object *o = typed("turn.end");
    json_object_object_add(o, "completed", json_object_new_boolean(completed));
    post_turn(user, o);
}

static const mr_turn_events turn_events = { .token = on_token, .audio = on_audio, .error = on_turn_error, .end = on_turn_end };
static struct turn_ctx *turn_ctx;       /* the running turn's; one turn at a time */

/* The worker is done (ended or cancelled): the exchange joins the history and Thread. */
static void close_turn(mr_daemon *d, bool cancelled) {
    if (!d->turn.worker) return;
    if (cancelled) {
        atomic_store(&d->turn_stopping, true);
        mr_turn_cancel(d->turn.worker);
        if (d->audio) mv_audio_stop_playback(d->audio);
    }
    atomic_store(&d->turn_gen, ++d->turn.gen);     /* whatever the worker still posts is stale */
    mr_turn_free(d->turn.worker);
    atomic_store(&d->turn_stopping, false);
    d->turn.worker = NULL;
    free(turn_ctx);
    turn_ctx = NULL;
    const char *reply = d->turn.reply.data ? (char *)d->turn.reply.data : "";
    if (*reply) mb_history_append(&d->history, MB_ROLE_ASSISTANT, reply);
    if (*reply || !d->turn.failed) deposit(d, cancelled);
    struct json_object *o = typed("reply.end");
    json_object_object_add(o, "cancelled", json_object_new_boolean(cancelled));
    broadcast(d, o);
}

static void reset_turn(mr_daemon *d) {
    free(d->turn.question);
    d->turn.question = NULL;
    mc_buf_free(&d->turn.reply);
    d->turn.failed = d->turn.audio_started = d->turn.draining = false;
    d->turn.quiet_since = 0;
}

static void start_turn(mr_daemon *d, const char *question, bool voice) {
    close_turn(d, true);
    reset_turn(d);
    mr_ears_mute(d->ears);
    microphone(d, false);
    d->turn.question = strdup(question);
    d->turn.voice = voice;
    d->turn.started_wall = mc_wall_ms();
    mb_history_append(&d->history, MB_ROLE_USER, question);

    struct json_object *said = typed("transcript");
    json_object_object_add(said, "text", json_object_new_string(question));
    json_object_object_add(said, "final", json_object_new_boolean(1));
    json_object_object_add(said, "source", json_object_new_string(voice ? "voice" : "typed"));
    broadcast(d, said);

    mb_clock clock;
    mb_clock_now(&clock);
    char *instructions = mb_sewn_instructions(&clock);
    char request_id[64];
    mt_turn_document_id(d->turn.started_wall, request_id, sizeof request_id);
    mb_turn_request req = { .instructions = instructions, .owner_id = d->owner, .request_id = request_id };
    struct json_object *start = mb_turn_start(&d->history, &req);
    free(instructions);

    turn_ctx = calloc(1, sizeof *turn_ctx);
    int error = 0;
    if (turn_ctx) {
        turn_ctx->d = d;
        turn_ctx->gen = d->turn.gen;
        d->turn.worker = mr_turn_start(d->sewn_path, start, &turn_events, turn_ctx, &error);
    }
    json_object_put(start);
    if (!d->turn.worker) {
        free(turn_ctx);
        turn_ctx = NULL;
        broadcast(d, error_message("sewnd", error == -ENOENT || error == -ECONNREFUSED ? "sewnd is not running" : strerror(-error)));
        set_state(d, MV_STATE_ERROR);
        standby(d);
        return;
    }
    set_state(d, MV_STATE_THINKING);
}

static void stop_everything(mr_daemon *d) {
    close_turn(d, true);
    if (d->audio) mv_audio_stop_playback(d->audio);
    d->turn.draining = false;
    standby(d);
    set_state(d, MV_STATE_IDLE);
}

static void on_turn_event(mr_daemon *d, const char *type, struct json_object *ev) {
    int64_t gen = -1;
    if (!mc_json_int64(ev, "gen", &gen) || gen != d->turn.gen || !d->turn.worker) return;
    if (strcmp(type, "turn.token") == 0) {
        const char *text = mc_json_string(ev, "text");
        mc_buf_append_str(&d->turn.reply, text);
        struct json_object *o = typed("reply.delta");
        json_object_object_add(o, "text", json_object_new_string(text));
        broadcast(d, o);
    } else if (strcmp(type, "turn.audio") == 0) {
        d->turn.audio_started = true;
        set_state(d, MV_STATE_SPEAKING);
    } else if (strcmp(type, "turn.error") == 0) {
        d->turn.failed = true;
        broadcast(d, error_message(mc_json_string(ev, "stage"), mc_json_string(ev, "message")));
    } else if (strcmp(type, "turn.end") == 0) {
        bool completed = false;
        mc_json_bool(ev, "completed", &completed);
        bool failed = d->turn.failed && !d->turn.reply.len;
        close_turn(d, false);
        if (failed) {
            set_state(d, MV_STATE_ERROR);
            standby(d);
        } else {
            d->turn.draining = true;        /* tick waits for the speaker */
        }
    }
}

/* ---- the ears ---- */

static void on_ears_event(mr_daemon *d, const char *type, struct json_object *ev) {
    int64_t session = 0;
    mc_json_int64(ev, "session", &session);
    bool turn_running = d->turn.worker || d->turn.draining;
    if (strcmp(type, "ears.wake") == 0) {
        if (turn_running) return;
        microphone(d, true);
        broadcast(d, typed("wake"));
    } else if (strcmp(type, "ears.state") == 0) {
        const char *name = mc_json_string(ev, "state");
        mv_state state;
        if (turn_running || !name || !mv_state_from_name(name, &state)) return;
        if (state == MV_STATE_LISTENING) d->ears_session = (int)session;
        if (session == d->ears_session) set_state(d, state);
    } else if (strcmp(type, "ears.level") == 0) {
        if (d->state != MV_STATE_LISTENING && d->state != MV_STATE_HEARING) return;
        struct json_object *o = typed("level");
        double rms = 0;
        mc_json_double(ev, "rms", &rms);
        json_object_object_add(o, "rms", json_object_new_double(rms));
        broadcast(d, o);
    } else if (session != d->ears_session || turn_running) {
        return;
    } else if (strcmp(type, "ears.partial") == 0) {
        struct json_object *o = typed("transcript");
        json_object_object_add(o, "text", json_object_new_string(mc_json_string(ev, "text")));
        json_object_object_add(o, "final", json_object_new_boolean(0));
        broadcast(d, o);
    } else if (strcmp(type, "ears.heard") == 0) {
        bool woke = false;
        mc_json_bool(ev, "woke", &woke);
        char request[4096];
        switch (mr_heard(mc_json_string(ev, "text"), woke, request, sizeof request)) {
        case MR_HEARD_REQUEST:
            start_turn(d, request, true);
            break;
        case MR_HEARD_NOTHING:
            if (woke) {
                listen_now(d, false);       /* "Hey Mary." — she is listening now */
                break;
            }
            /* fall through */
        case MR_HEARD_STOP:
            standby(d);
            set_state(d, MV_STATE_IDLE);
            break;
        }
    } else if (strcmp(type, "ears.timeout") == 0) {
        microphone(d, false);
        set_state(d, MV_STATE_IDLE);
    } else if (strcmp(type, "ears.error") == 0) {
        broadcast(d, error_message(mc_json_string(ev, "stage"), mc_json_string(ev, "message")));
        microphone(d, false);
        set_state(d, MV_STATE_ERROR);
    }
}

/* ---- skills ---- */

struct skill_call {
    mr_daemon *d;
    unsigned client;
};

static int send_to_desktop(struct json_object *message, void *user) {
    mr_daemon *d = user;
    mr_client *c = mr_desktop_the_desktop(&d->desktop);
    return c ? mr_desktop_send(&d->desktop, c, message) : -ENOTCONN;
}

static struct json_object *skill_result(const char *call_id, bool ok, const char *error, struct json_object *result) {
    struct json_object *o = typed("skill.result");
    if (call_id) json_object_object_add(o, "call_id", json_object_new_string(call_id));
    json_object_object_add(o, "ok", json_object_new_boolean(ok));
    if (ok) json_object_object_add(o, "result", result ? json_object_get(result) : NULL);
    else json_object_object_add(o, "error", json_object_new_string(error));
    return o;
}

static void on_skill_done(const mcu_result *r, void *user) {
    struct skill_call *call = user;
    mr_client *c = mr_desktop_client(&call->d->desktop, call->client);
    if (c) send_to(call->d, c, skill_result(r->call_id, r->ok, r->error, r->result));
    free(call);
}

static void skill_call(mr_daemon *d, mr_client *c, struct json_object *msg) {
    const char *app = mc_json_string(msg, "app"), *skill = mc_json_string(msg, "skill");
    if (!app || !skill) {
        send_to(d, c, skill_result(NULL, false, "unknown", NULL));
        return;
    }
    sk_decision decision = sk_registry_decide(&d->skills, app, skill);
    if (decision != SK_ALLOWED) {
        send_to(d, c, skill_result(NULL, false, sk_decision_name(decision), NULL));
        return;
    }
    struct skill_call *call = malloc(sizeof *call);
    if (!call) return;
    *call = (struct skill_call){ d, c->id };
    if (mcu_invoke(d->pipes, app, skill, mc_json_object(msg, "args"), d->config.skill_timeout_ms, on_skill_done, call, NULL, 0) < 0) {
        free(call);
        send_to(d, c, skill_result(NULL, false, "failed", NULL));
    }
}

/* ---- the desktop's messages ---- */

static void on_message(mr_desktop *desktop, mr_client *c, struct json_object *msg, void *user) {
    mr_daemon *d = user;
    const char *type = mc_json_type(msg);
    if (strcmp(type, "ask") == 0) {
        const char *text = mc_json_string(msg, "text");
        if (text && *text) start_turn(d, text, false);
    } else if (strcmp(type, "listen") == 0) {
        close_turn(d, true);
        d->turn.draining = false;
        listen_now(d, false);
    } else if (strcmp(type, "stop") == 0 || strcmp(type, "dismiss") == 0) {
        stop_everything(d);
    } else if (strcmp(type, "key.set") == 0) {
        struct json_object *field = NULL;
        const char *key = json_object_object_get_ex(msg, "key", &field) && json_object_is_type(field, json_type_string)
                              ? json_object_get_string(field) : NULL;
        size_t len = key ? strlen(key) : 0;
        if (!key || !sewn_key_valid(key, len)) send_to(d, c, error_message("key", "That does not look like a Mistral API key."));
        else key_job(d, "key.set", false, key, len);
        if (key) mc_secure_zero((char *)key, len);      /* json-c's copy of it */
    } else if (strcmp(type, "key.verify") == 0 || strcmp(type, "key.status") == 0) {
        key_job(d, type, false, NULL, 0);
    } else if (strcmp(type, "config") == 0) {
        bool wake = true;
        if (mc_json_bool(msg, "wake", &wake)) {
            mr_ears_set_wake(d->ears, wake);
            if (d->state == MV_STATE_IDLE) microphone(d, false);
        }
    } else if (strcmp(type, "skills") == 0) {
        if (sk_registry_load(&d->skills, msg) < 0) {
            send_to(d, c, error_message("skills", "a skills message that could not be read"));
            return;
        }
        if (d->skills_message) json_object_put(d->skills_message);
        d->skills_message = json_object_get(msg);
        for (int i = 0; i < MR_CLIENTS_MAX; i++) d->desktop.clients[i].desktop = false;
        c->desktop = true;
    } else if (strcmp(type, "skill.result") == 0) {
        mcu_pipes_on_message(d->pipes, msg);
    } else if (strcmp(type, "skills.list") == 0) {
        struct json_object *o = typed("skills"), *apps = d->skills_message ? mc_json_array(d->skills_message, "apps") : NULL;
        json_object_object_add(o, "apps", apps ? json_object_get(apps) : json_object_new_array());
        send_to(d, c, o);
    } else if (strcmp(type, "skill.call") == 0) {
        skill_call(d, c, msg);
    } else {
        char message[128];
        snprintf(message, sizeof message, "maryd does not know \"%.60s\"", type);
        send_to(d, c, error_message("request", message));
    }
}

static void on_client(mr_desktop *desktop, mr_client *c, bool connected, void *user) {
    mr_daemon *d = user;
    if (!connected) {
        if (c->desktop) mcu_pipes_disconnect(d->pipes);
        return;
    }
    struct json_object *hello = typed("hello");
    json_object_object_add(hello, "state", json_object_new_string(mv_state_name(d->state)));
    json_object_object_add(hello, "key_present", json_object_new_boolean(d->key_present));
    json_object_object_add(hello, "wake", json_object_new_boolean(mr_ears_can_wake(d->ears)));
    json_object_object_add(hello, "tail", mb_history_spoken_messages(&d->history));
    send_to(d, c, hello);
    if (!d->key_present) key_job(d, "key.status", true, NULL, 0);
}

static void on_key_reply(mr_daemon *d, struct json_object *ev) {
    bool quiet = false;
    mc_json_bool(ev, "quiet", &quiet);
    struct json_object *reply = mc_json_object(ev, "reply");
    const char *failure = mc_json_string(ev, "failure"), *type = reply ? mc_json_type(reply) : NULL;
    if (failure) {
        if (!quiet) broadcast(d, error_message("sewnd", failure));
    } else if (type && strcmp(type, "key.status") == 0) {
        mc_json_bool(reply, "present", &d->key_present);
        mr_desktop_broadcast(&d->desktop, reply);
    } else if (type && strcmp(type, "error") == 0) {
        mr_desktop_broadcast(&d->desktop, reply);
    }
}

/* ---- the loop ---- */

static void on_capture(const int16_t *frame, size_t count, void *user) { mr_daemon_hear(user, frame, count); }

static void tick(mr_daemon *d) {
    mcu_pipes_tick(d->pipes, mc_now_ms());
    if (!d->turn.draining) return;
    if (d->audio && mv_audio_queued(d->audio) > 0) {
        d->turn.quiet_since = 0;
        return;
    }
    int64_t now = mc_now_ms();
    if (!d->turn.quiet_since) d->turn.quiet_since = now;
    if (d->turn.audio_started && now - d->turn.quiet_since < d->config.echo_tail_ms) return;
    d->turn.draining = false;
    if (d->turn.voice) {
        listen_now(d, true);
    } else {
        standby(d);
        set_state(d, MV_STATE_IDLE);
    }
}

static void resolve(char *out, size_t cap, const char *given, const char *env, const char *fallback) {
    const char *from_env = getenv(env);
    snprintf(out, cap, "%s", given ? given : from_env && *from_env ? from_env : fallback);
}

mr_daemon *mr_daemon_new(const mr_config *config, int *error) {
    mr_daemon *d = calloc(1, sizeof *d);
    int rc = d ? 0 : -ENOMEM;
    if (rc == 0) {
        d->config = *config;
        if (d->config.listen_timeout_ms <= 0) d->config.listen_timeout_ms = 6000;
        if (d->config.echo_tail_ms < 0) d->config.echo_tail_ms = 300;
        if (d->config.skill_timeout_ms <= 0) d->config.skill_timeout_ms = 10000;
        resolve(d->sewn_path, sizeof d->sewn_path, config->sewn_socket, "SEWN_SOCKET", SEWN_SOCKET_PATH);
        resolve(d->thread_path, sizeof d->thread_path, config->thread_socket, "THREAD_SOCKET", MT_SOCKET_PATH);
        if (config->desktop_socket) snprintf(d->desktop_path, sizeof d->desktop_path, "%s", config->desktop_socket);
        else rc = mr_desktop_default_socket(d->desktop_path, sizeof d->desktop_path, true);
        mc_user_name(getuid(), d->owner, sizeof d->owner);
        atomic_init(&d->stop, false);
        atomic_init(&d->workers, 0);
        atomic_init(&d->turn_stopping, false);
        atomic_init(&d->turn_gen, 0);
        d->state = MV_STATE_IDLE;
        mb_history_init(&d->history);
        sk_registry_init(&d->skills);
    }
    if (rc == 0) rc = mr_queue_init(&d->queue);
    if (rc == 0 && !(d->pipes = mcu_pipes_new(send_to_desktop, d))) rc = -ENOMEM;
    if (rc == 0) rc = mr_desktop_listen(&d->desktop, d->desktop_path, on_message, on_client, d);
    if (rc == 0) {
        mr_ears_config ears = { .sewn_socket = d->sewn_path, .kws = config->kws, .wake = config->wake,
                                .listen_timeout_ms = d->config.listen_timeout_ms };
        d->ears = mr_ears_new(&ears, &d->queue, &rc);
    }
    if (rc < 0) {
        if (error) *error = rc;
        mr_daemon_free(d);
        return NULL;
    }
    if (config->audio) {
        int audio_error = 0;
        mv_audio_config audio = mv_audio_config_default();
        d->audio = mv_audio_open(&audio, on_capture, d, &audio_error);
        if (!d->audio) mc_log(MC_LOG_WARNING, "no microphone or speaker: %s", strerror(-audio_error));
        microphone(d, false);
    }
    key_job(d, "key.status", true, NULL, 0);
    return d;
}

int mr_daemon_run(mr_daemon *d) {
    struct pollfd fds[2 + MR_CLIENTS_MAX];
    while (!atomic_load(&d->stop)) {
        fds[0] = (struct pollfd){ .fd = mr_queue_fd(&d->queue), .events = POLLIN };
        size_t n = 1 + mr_desktop_pollfds(&d->desktop, fds + 1, sizeof fds / sizeof *fds - 1);
        int timeout = d->turn.draining || mcu_pipes_pending(d->pipes) ? 50 : 1000;
        if (poll(fds, n, timeout) < 0 && errno != EINTR) break;
        mr_queue_drain(&d->queue);
        struct json_object *ev;
        while (!atomic_load(&d->stop) && (ev = mr_queue_pop(&d->queue))) {
            const char *type = mc_json_type(ev);
            if (!type) {
            } else if (strncmp(type, "turn.", 5) == 0) {
                on_turn_event(d, type, ev);
            } else if (strncmp(type, "ears.", 5) == 0) {
                on_ears_event(d, type, ev);
            } else if (strcmp(type, "key.reply") == 0) {
                on_key_reply(d, ev);
            }
            json_object_put(ev);
        }
        mr_desktop_handle(&d->desktop, fds + 1, n - 1);
        tick(d);
    }
    return 0;
}

void mr_daemon_stop(mr_daemon *d) {
    atomic_store(&d->stop, true);
    mr_queue_wake(&d->queue);
}

void mr_daemon_hear(mr_daemon *d, const int16_t *samples, size_t count) {
    if (d->ears) mr_ears_hear(d->ears, samples, count);
}

void mr_daemon_free(mr_daemon *d) {
    if (!d) return;
    if (d->audio) mv_audio_capture(d->audio, false);
    close_turn(d, true);
    reset_turn(d);
    mr_ears_free(d->ears);
    mv_audio_close(d->audio);
    for (int i = 0; i < 600 && atomic_load(&d->workers) > 0; i++) usleep(10000);
    if (d->pipes) mcu_pipes_free(d->pipes);
    if (d->desktop.path[0]) mr_desktop_close(&d->desktop);
    if (atomic_load(&d->workers) == 0 && d->queue.items) mr_queue_free(&d->queue);
    if (d->skills_message) json_object_put(d->skills_message);
    sk_registry_free(&d->skills);
    mb_history_free(&d->history);
    free(d);
}
