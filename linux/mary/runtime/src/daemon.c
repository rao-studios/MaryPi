#include "runtime/daemon.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "ambient/engine.h"
#include "ambient/prompt.h"
#include "ambient/realm.h"
#include "ambient/store.h"
#include "ambient/trace.h"
#include "ambient/wire.h"
#include "brain/clock.h"
#include "brain/lane.h"
#include "brain/scope.h"
#include "brain/triage.h"
#include "common/jsonl.h"
#include "foundation/behavior.h"
#include "brain/history.h"
#include "brain/prompt.h"
#include "brain/request.h"
#include "common/io.h"
#include "common/json.h"
#include "common/log.h"
#include "common/peer.h"
#include "common/secure.h"
#include "computer-use/invoke.h"
#include "thread/client.h"
#include "thread/service.h"
#include "runtime/desktop.h"
#include "runtime/ears.h"
#include "runtime/queue.h"
#include "runtime/turn.h"
#include "sewn/client.h"
#include "sewn/key.h"
#include "sewn/voices.h"
#include "skills/registry.h"
#include "voice/audio.h"
#include "voice/state.h"

struct mr_daemon {
    mr_config config;
    char desktop_path[256], sewn_path[256], owner[64];
    char thread_local_path[256];
    mr_queue queue;
    mr_desktop desktop;
    mr_ears *ears;
    void *audio;                    /* through aops; NULL while there is no speaker */
    const mr_audio_ops *aops;
    int64_t audio_retry_at;
    int audio_backoff_ms;
    mcu_pipes *pipes;
    sk_registry skills;
    struct json_object *skills_message;
    mb_history history;
    atomic_bool stop;
    atomic_int workers;
    mv_state state;
    bool key_present;
    int ears_session;
    char voice_id[64];              /* what turns are spoken in: config{voice}, MB_VOICE_ID until then */
    char voice_engine[16], skill_engine[16];    /* the engine of each lane: config{voice_engine, skill_engine}; mistral until then */
    struct json_object *voices;     /* sewnd's last list of voices, served for ten minutes */
    int64_t voices_at;

    /* The ambient world (PARITY D28, D29): the roster of applications, what is in front of the person, where they
     * have been, and what every turn decided. */
    ma_roster roster;
    ma_store *ambient;
    ma_focus_ledger focus;
    ma_trace_log *trace;
    /* Abilities (M7): the skill index triage scores against, the lane in flight, and the episode. */
    mb_skill_index skill_index;
    pthread_mutex_t index_lock;
    bool index_ready;
    struct lane_bridge *bridge;     /* Lane B, while it runs */
    mf_behavior_episode episode;
    bool episode_open;
    char prior_episode_id[MF_UUID_LEN + 1];
    mb_recall recall;
    struct {
        bool active;                /* a turn waiting for the desktop's world (at most 150 ms) */
        char *question;
        bool voice;
        int64_t deadline;
    } pending;

    struct {
        mr_turn *worker;
        int gen;
        bool voice, failed, audio_started, draining, speaker_reported;
        char *question;
        mc_buf reply;
        struct json_object *contribution, *retrieved;   /* sewnd's turn.end: Gita's contribution and what was retrieved */
        int64_t started_wall, quiet_since, audio_started_ms;
        char request_id[64];
        char lanes[4][16];          /* the storage lanes the context was asked from */
        int lane_count;
        struct json_object *start;  /* turn.start, built before triage decides which lane runs */
        struct json_object *runs;   /* the skill runs of this turn, for reply.end */
        ma_intent intent;
        bool action_turn, implies_action;
        char *system_prompt;        /* Lane B's, built with the turn */
        struct json_object *scope;  /* Lane B's scope */
        bool spoken_reply;          /* the reply is being spoken (Lane B's answer, or a dispatch's receipt) */
        bool open;                  /* begun and not yet ended: reply.end is owed */
        bool triaging;              /* waiting for the triage worker */
        char lane[16];              /* voice | orchestrator | embedding */
        char lead_name[64];
    } turn;
    int skills_gen;                 /* bumped on every skills message; an index built for an older one is dropped */
    int64_t index_retry_at;         /* 0: none pending; else when to build the index again (sewnd was not reachable, or had no key) */
    int index_tries;
    int64_t styles_retry_at;        /* 0: none pending; else when to deposit the style records again (threadd was not reachable) */
    int styles_tries;
    atomic_bool turn_stopping;      /* the audio callback gives up waiting for room */
    atomic_int turn_gen;
    struct {
        mr_turn *worker;
        int gen;
        unsigned client;            /* who asked */
        char voice_id[64];
        bool failed, audio_started, draining;
        int64_t audio_started_ms;
    } sample;                       /* one text in a voice (Settings' Play Sample): out of the conversation */
    atomic_int sample_gen;
};

mr_config mr_config_default(void) {
    return (mr_config){ .audio = true, .wake = true, .listen_timeout_ms = 6000, .echo_tail_ms = 300,
                        .skill_timeout_ms = 10000, .speaker_stall_ms = 2000 };
}

/* PipeWire, as maryd's audio. */
static void *pw_open(mr_capture_fn on_frame, void *user, int *error, void *ops_user) {
    mv_audio_config config = mv_audio_config_default();
    return mv_audio_open(&config, on_frame, user, error);
}
static void pw_close(void *audio) { mv_audio_close(audio); }
static int pw_capture(void *audio, bool on) { return mv_audio_capture(audio, on); }
static size_t pw_play(void *audio, const float *samples, size_t count) { return mv_audio_play(audio, samples, count); }
static void pw_stop(void *audio) { mv_audio_stop_playback(audio); }
static size_t pw_queued(void *audio) { return mv_audio_queued(audio); }
static int64_t pw_last_played_ms(void *audio) { return mv_audio_last_played_ms(audio); }
static bool pw_broken(void *audio) { return mv_audio_broken(audio); }
static const mr_audio_ops pipewire_ops = { pw_open, pw_close, pw_capture, pw_play, pw_stop, pw_queued, pw_last_played_ms, pw_broken };

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

static double wall_seconds(void) { return (double)mc_wall_ms() / 1000.0; }

static void set_state(mr_daemon *d, mv_state state) {
    if (d->state == state) return;
    d->state = state;
    struct json_object *o = typed("state");
    json_object_object_add(o, "state", json_object_new_string(mv_state_name(state)));
    broadcast(d, o);
}

/* The microphone is open only while it could matter: spotting the name, or a session. */
static void microphone(mr_daemon *d, bool session) {
    if (d->audio) d->aops->capture(d->audio, session || mr_ears_can_wake(d->ears));
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
    bool voices;            /* voices.list for `client`, not the key */
    bool calls;             /* calls.list{limit} for `client`: sewnd's ledger */
    int limit;
    unsigned client;
};

static void *key_worker(void *arg) {
    struct key_job *job = arg;
    mr_daemon *d = job->d;
    struct json_object *reply = NULL, *event = typed(job->voices ? "voices.reply" : job->calls ? "calls.reply" : "key.reply");
    int fd = sewn_connect(d->sewn_path), rc = fd;
    if (fd >= 0) {
        struct timeval patience = { .tv_sec = job->voices ? 45 : 20 };   /* five pages of voices take longer */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof patience);
        if (job->key) {
            rc = sewn_call_key_set(fd, job->key, job->key_len, &reply);
        } else {
            struct json_object *request = typed(job->type);
            if (job->calls) json_object_object_add(request, "limit", json_object_new_int(job->limit));
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
    json_object_object_add(event, "client", json_object_new_int64(job->client));
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

static void calls_job(mr_daemon *d, mr_client *c, int limit) {
    struct key_job *job = calloc(1, sizeof *job);
    if (!job) return;
    job->d = d;
    snprintf(job->type, sizeof job->type, "calls.list");
    job->calls = true;
    job->limit = limit;
    job->client = c->id;
    spawn(d, key_worker, job);
}

static void voices_job(mr_daemon *d, mr_client *c) {
    struct key_job *job = calloc(1, sizeof *job);
    if (!job) return;
    job->d = d;
    snprintf(job->type, sizeof job->type, "voices.list");
    job->voices = true;
    job->client = c->id;
    spawn(d, key_worker, job);
}

/* ---- deposits on the local socket: style, behaviour and routing records ---- */

struct items_job {
    mr_daemon *d;
    struct json_object *items;      /* an array of deposit requests */
    char what[32];
};

static void *items_worker(void *arg) {
    struct items_job *job = arg;
    size_t n = json_object_array_length(job->items), ok = 0;
    for (size_t i = 0; i < n; i++) {
        struct json_object *reply = NULL;
        char message[200] = "";
        int rc = mc_jsonl_call(job->d->thread_local_path, THREAD_CLIENT_TIMEOUT_MS, json_object_array_get_idx(job->items, i), &reply, message, sizeof message);
        if (rc == 0) ok++;
        else if (i == 0) mc_log(MC_LOG_WARNING, "%s records not deposited: %s", job->what, message[0] ? message : strerror(-rc));
        if (reply) json_object_put(reply);
        if (rc == -ENOENT || rc == -ECONNREFUSED) break;
    }
    if (ok) mc_log(MC_LOG_DEBUG, "deposited %zu %s record%s", ok, job->what, ok == 1 ? "" : "s");
    /* the main loop hears how it went, so records threadd could not take yet are offered again */
    struct json_object *ev = typed("deposit.done");
    json_object_object_add(ev, "what", json_object_new_string(job->what));
    json_object_object_add(ev, "ok", json_object_new_boolean(ok == n));
    json_object_object_add(ev, "unreachable", json_object_new_boolean(ok == 0 && n > 0));
    mr_daemon *d = job->d;
    json_object_put(job->items);
    free(job);
    mr_queue_push(&d->queue, ev);
    atomic_fetch_sub(&d->workers, 1);
    return NULL;
}

/* Deposits an array of records (takes the reference). */
static void deposit_items(mr_daemon *d, struct json_object *items, const char *what) {
    if (!items || !json_object_array_length(items)) { if (items) json_object_put(items); return; }
    struct items_job *job = calloc(1, sizeof *job);
    if (!job) { json_object_put(items); return; }
    job->d = d;
    job->items = items;
    snprintf(job->what, sizeof job->what, "%s", what);
    spawn(d, items_worker, job);
}

/* ---- the skill index: built when the desktop publishes its skills ---- */

struct index_job {
    mr_daemon *d;
    struct json_object *message;    /* a private copy of the skills message */
    int gen;                        /* the registry it stands for */
};

static void *index_worker(void *arg) {
    struct index_job *job = arg;
    mr_daemon *d = job->d;
    char message[200] = "";
    sk_registry registry;
    sk_registry_init(&registry);
    sk_registry_load(&registry, job->message);
    mb_skill_index built;
    memset(&built, 0, sizeof built);
    int rc = mb_skill_index_build(&built, &registry, mb_embed_sewn, d->sewn_path, message, sizeof message);
    sk_registry_free(&registry);
    json_object_put(job->message);
    pthread_mutex_lock(&d->index_lock);
    bool current = job->gen == d->skills_gen;
    if (current) {
        mb_skill_index_free(&d->skill_index);
        d->skill_index = built;
        d->index_ready = rc == 0 && built.n > 0;
    } else {
        mb_skill_index_free(&built);
    }
    pthread_mutex_unlock(&d->index_lock);
    if (rc < 0) mc_log(MC_LOG_WARNING, "the skill index was not built: %s", message);
    else if (current) mc_log(MC_LOG_DEBUG, "skill index: %zu skill%s", built.n, built.n == 1 ? "" : "s");
    struct json_object *ev = typed("index.done");
    json_object_object_add(ev, "ok", json_object_new_boolean(rc == 0));
    json_object_object_add(ev, "current", json_object_new_boolean(current));
    /* sewnd not up yet: worth another try soon. Without a key (sewnd's own refusal, -EIO) the build waits for the key. */
    json_object_object_add(ev, "retryable", json_object_new_boolean(rc == -ENOENT || rc == -ECONNREFUSED || rc == -EAGAIN || rc == -EPIPE));
    free(job);
    mr_queue_push(&d->queue, ev);
    atomic_fetch_sub(&d->workers, 1);
    return NULL;
}

/* Builds the skill index from the registry as last published (a private copy of the message). */
static void build_index(mr_daemon *d) {
    if (!d->skills_message) return;
    pthread_mutex_lock(&d->index_lock);
    int skills_gen = d->skills_gen;
    pthread_mutex_unlock(&d->index_lock);
    struct index_job *job = calloc(1, sizeof *job);
    if (job && json_object_deep_copy(d->skills_message, &job->message, NULL) == 0) {
        job->d = d;
        job->gen = skills_gen;
        spawn(d, index_worker, job);
    } else {
        free(job);
    }
}

/* ---- Lane B's bridge to the main loop ---- */

struct lane_bridge {
    mr_daemon *d;
    int gen;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    /* an invoke in flight */
    bool invoke_done;
    int invoke_rc;
    struct json_object *invoke_result;
    char invoke_error[120];
    /* a confirmation in flight */
    bool confirm_pending, confirm_done;
    int confirm_answer;
    char confirm_call_id[64];
    atomic_bool cancelled;
    /* what the lane runs with */
    struct json_object *messages, *scope;
    char *system;
    sk_registry registry;           /* frozen for the lane */
    bool implies_action;
    char request_id[64];
    char provider[16];
};

static void bridge_free(struct lane_bridge *b) {
    if (!b) return;
    if (b->invoke_result) json_object_put(b->invoke_result);
    if (b->messages) json_object_put(b->messages);
    if (b->scope) json_object_put(b->scope);
    free(b->system);
    sk_registry_free(&b->registry);
    pthread_mutex_destroy(&b->lock);
    pthread_cond_destroy(&b->cond);
    free(b);
}

static int wait_until(struct lane_bridge *b, bool *flag, int timeout_ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    int rc = 0;
    while (!*flag && rc == 0 && !atomic_load(&b->cancelled)) rc = pthread_cond_timedwait(&b->cond, &b->lock, &deadline);
    return *flag ? 0 : atomic_load(&b->cancelled) ? -ECANCELED : -ETIMEDOUT;
}

static int lane_complete(struct json_object *request, struct json_object **reply, char *message, size_t cap, void *user) {
    struct lane_bridge *b = user;
    int fd = sewn_connect(b->d->sewn_path);
    if (fd < 0) { snprintf(message, cap, "sewnd is not running"); return fd; }
    struct timeval patience = { .tv_sec = 90 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof patience);
    int rc = sewn_call(fd, request, reply);
    close(fd);
    if (rc < 0) { snprintf(message, cap, "sewnd: %s", strerror(-rc)); return rc; }
    const char *type = mc_json_type(*reply);
    if (!type || strcmp(type, "complete.result") != 0) {
        const char *why = mc_json_string(*reply, "message");
        snprintf(message, cap, "%s", why ? why : "sewnd could not complete");
        json_object_put(*reply);
        *reply = NULL;
        return -EIO;
    }
    return 0;
}

/* The lane thread asks the main loop to run a skill, and waits. */
static int lane_invoke(const char *app, const char *skill, struct json_object *args, struct json_object **result, char *error, size_t cap, void *user) {
    struct lane_bridge *b = user;
    pthread_mutex_lock(&b->lock);
    b->invoke_done = false;
    if (b->invoke_result) json_object_put(b->invoke_result);
    b->invoke_result = NULL;
    b->invoke_error[0] = 0;
    struct json_object *ev = typed("lane.invoke");
    json_object_object_add(ev, "gen", json_object_new_int(b->gen));
    json_object_object_add(ev, "app", json_object_new_string(app));
    json_object_object_add(ev, "skill", json_object_new_string(skill));
    json_object_object_add(ev, "args", args ? json_object_get(args) : json_object_new_object());
    mr_queue_push(&b->d->queue, ev);
    int rc = wait_until(b, &b->invoke_done, b->d->config.skill_timeout_ms + 2000);
    if (rc == 0) {
        rc = b->invoke_rc;
        *result = b->invoke_result;
        b->invoke_result = NULL;
        if (rc < 0) snprintf(error, cap, "%s", b->invoke_error[0] ? b->invoke_error : strerror(-rc));
    } else {
        snprintf(error, cap, "%s", rc == -ECANCELED ? "cancelled" : "timeout");
    }
    pthread_mutex_unlock(&b->lock);
    return rc;
}

/* The lane thread asks the person, through the desktop's card, and waits up to a minute. */
static int lane_confirm(const char *call_id, const sk_app *app, const sk_skill *skill, struct json_object *args, const char *summary, void *user) {
    struct lane_bridge *b = user;
    pthread_mutex_lock(&b->lock);
    b->confirm_done = false;
    b->confirm_pending = true;
    snprintf(b->confirm_call_id, sizeof b->confirm_call_id, "%s", call_id);
    struct json_object *ev = typed("lane.confirm");
    json_object_object_add(ev, "gen", json_object_new_int(b->gen));
    json_object_object_add(ev, "call_id", json_object_new_string(call_id));
    json_object_object_add(ev, "app", json_object_new_string(app->id));
    json_object_object_add(ev, "app_name", json_object_new_string(app->name));
    json_object_object_add(ev, "skill", json_object_new_string(skill->id));
    json_object_object_add(ev, "title", json_object_new_string(skill->title));
    json_object_object_add(ev, "args", args ? json_object_get(args) : json_object_new_object());
    json_object_object_add(ev, "summary", json_object_new_string(summary));
    mr_queue_push(&b->d->queue, ev);
    int rc = wait_until(b, &b->confirm_done, 60000);
    int answer = rc == 0 ? b->confirm_answer : rc;
    b->confirm_pending = false;
    pthread_mutex_unlock(&b->lock);
    return answer;
}

static bool lane_cancelled(void *user) { return atomic_load(&((struct lane_bridge *)user)->cancelled); }

static void lane_run_started(const mb_lane_outcome *o, void *user) {
    struct lane_bridge *b = user;
    struct json_object *ev = typed("lane.run");
    json_object_object_add(ev, "gen", json_object_new_int(b->gen));
    json_object_object_add(ev, "app", json_object_new_string(o->app));
    json_object_object_add(ev, "skill", json_object_new_string(o->skill));
    json_object_object_add(ev, "invocation", json_object_new_string(o->invocation));
    json_object_object_add(ev, "args", json_object_new_string(o->arguments));
    json_object_object_add(ev, "requested", json_object_new_boolean(o->requested));
    mr_queue_push(&b->d->queue, ev);
}

static struct json_object *outcome_json(const mb_lane_outcome *o) {
    struct json_object *r = json_object_new_object();
    json_object_object_add(r, "call_id", json_object_new_string(o->call_id));
    json_object_object_add(r, "app", json_object_new_string(o->app));
    json_object_object_add(r, "skill", json_object_new_string(o->skill));
    json_object_object_add(r, "invocation", json_object_new_string(o->invocation));
    json_object_object_add(r, "args", json_object_new_string(o->arguments));
    json_object_object_add(r, "ok", json_object_new_boolean(o->ok));
    json_object_object_add(r, "requested", json_object_new_boolean(o->requested));
    json_object_object_add(r, "found_nothing", json_object_new_boolean(o->found_nothing));
    json_object_object_add(r, "is_read", json_object_new_boolean(o->is_read));
    json_object_object_add(r, "summary", json_object_new_string(o->summary));
    json_object_object_add(r, "started", json_object_new_double(o->started));
    json_object_object_add(r, "finished", json_object_new_double(o->finished));
    return r;
}

static void lane_run_finished(const mb_lane_outcome *o, void *user) {
    struct lane_bridge *b = user;
    struct json_object *ev = typed("lane.result");
    json_object_object_add(ev, "gen", json_object_new_int(b->gen));
    json_object_object_add(ev, "outcome", outcome_json(o));
    mr_queue_push(&b->d->queue, ev);
}

static void *lane_thread(void *arg) {
    struct lane_bridge *b = arg;
    mb_lane_hooks hooks = { .complete = lane_complete, .invoke = lane_invoke, .confirm = lane_confirm, .run_started = lane_run_started,
                            .run_finished = lane_run_finished, .cancelled = lane_cancelled, .user = b };
    mb_lane_request req = { .system = b->system, .messages = b->messages, .registry = &b->registry, .scope = b->scope, .provider = b->provider,
                            .request_id = b->request_id, .implies_action = b->implies_action };
    mb_lane_result result;
    int rc = mb_lane_run(&req, &hooks, &result);
    struct json_object *ev = typed("lane.done");
    json_object_object_add(ev, "gen", json_object_new_int(b->gen));
    json_object_object_add(ev, "ok", json_object_new_boolean(rc == 0));
    json_object_object_add(ev, "text", json_object_new_string(result.text ? result.text : ""));
    json_object_object_add(ev, "error", json_object_new_string(rc == 0 ? result.error : (result.error[0] ? result.error : strerror(-rc))));
    json_object_object_add(ev, "rounds", json_object_new_int(result.rounds));
    json_object_object_add(ev, "cancelled", json_object_new_boolean(result.cancelled));
    struct json_object *outcomes = json_object_new_array();
    for (int i = 0; i < result.outcome_count; i++) json_object_array_add(outcomes, outcome_json(&result.outcomes[i]));
    json_object_object_add(ev, "outcomes", outcomes);
    mb_lane_result_free(&result);
    json_object_object_add(ev, "bridge", json_object_new_int64((int64_t)(intptr_t)b));
    mr_daemon *d = b->d;                /* the bridge is the main loop's to free once this is posted */
    mr_queue_push(&d->queue, ev);
    atomic_fetch_sub(&d->workers, 1);
    return NULL;
}

/* Lets the lane go: it ends on its own (every wait wakes on `cancelled`) and its lane.done frees the bridge. */
static void release_lane(mr_daemon *d) {
    struct lane_bridge *b = d->bridge;
    if (!b) return;
    d->bridge = NULL;
    pthread_mutex_lock(&b->lock);
    atomic_store(&b->cancelled, true);
    pthread_cond_broadcast(&b->cond);
    pthread_mutex_unlock(&b->lock);
}

/* The person answered the card (the desktop's button, or a bare yes or no). */
static bool answer_confirmation(mr_daemon *d, const char *call_id, bool yes) {
    struct lane_bridge *b = d->bridge;
    if (!b) return false;
    pthread_mutex_lock(&b->lock);
    bool pending = b->confirm_pending && !b->confirm_done && (!call_id || strcmp(call_id, b->confirm_call_id) == 0);
    if (pending) {
        b->confirm_answer = yes ? 1 : 0;
        b->confirm_done = true;
        pthread_cond_broadcast(&b->cond);
    }
    pthread_mutex_unlock(&b->lock);
    if (pending) ma_trace_note_confirmation(d->trace, d->turn.request_id, yes ? "allowed" : "refused");
    return pending;
}

static bool confirmation_pending(mr_daemon *d) {
    struct lane_bridge *b = d->bridge;
    if (!b) return false;
    pthread_mutex_lock(&b->lock);
    bool pending = b->confirm_pending && !b->confirm_done;
    pthread_mutex_unlock(&b->lock);
    return pending;
}

/* ---- triage: the one semantic read of a turn, on a worker (it embeds through sewnd) ---- */

struct triage_job {
    mr_daemon *d;
    int gen;
    unsigned client;                /* 0: the turn's; else maryctl / the Abilities app's Rehearse */
    char *text;
};

static void *triage_worker(void *arg) {
    struct triage_job *job = arg;
    mr_daemon *d = job->d;
    struct json_object *ev = typed("triage.done");
    json_object_object_add(ev, "gen", json_object_new_int(job->gen));
    json_object_object_add(ev, "client", json_object_new_int64((int64_t)job->client));
    json_object_object_add(ev, "text", json_object_new_string(job->text));
    float *vector = NULL;
    size_t dim = 0;
    char message[200] = "";
    const char *texts[1] = { job->text };
    int rc = mb_embed_sewn(texts, 1, &vector, &dim, message, sizeof message, d->sewn_path);
    pthread_mutex_lock(&d->index_lock);
    if (rc == 0 && (!d->index_ready || dim != d->skill_index.dim)) { rc = -ENOENT; snprintf(message, sizeof message, "no skill index"); }
    if (rc == 0) {
        mb_affinity affinities[8];
        int n = mb_affinities(&d->skill_index, vector, affinities, 8);
        struct json_object *list = json_object_new_array();
        for (int i = 0; i < n; i++) {
            struct json_object *a = json_object_new_object();
            json_object_object_add(a, "app", json_object_new_string(affinities[i].skill->app));
            json_object_object_add(a, "skill", json_object_new_string(affinities[i].skill->skill));
            json_object_object_add(a, "invocation", json_object_new_string(affinities[i].skill->invocation));
            json_object_object_add(a, "score", json_object_new_double(affinities[i].score));
            json_object_array_add(list, a);
        }
        json_object_object_add(ev, "affinities", list);
        const mb_skill_vector *winner = mb_unique_winner(affinities, n, &d->skills, MB_ROUTING_FLOOR, MB_ROUTING_MARGIN);
        const sk_skill *skill = winner ? sk_registry_skill(&d->skills, winner->app, winner->skill) : NULL;
        const sk_app *app = winner ? sk_registry_app(&d->skills, winner->app) : NULL;
        if (winner && skill && app) {
            mb_confidence_shape shape = mb_confidence_shape_of(skill, job->text);
            bool single = mb_is_single_clause(job->text);
            struct json_object *w = json_object_new_object();
            json_object_object_add(w, "app", json_object_new_string(winner->app));
            json_object_object_add(w, "skill", json_object_new_string(winner->skill));
            json_object_object_add(w, "invocation", json_object_new_string(winner->invocation));
            json_object_object_add(w, "title", json_object_new_string(skill->title ? skill->title : winner->skill));
            json_object_object_add(w, "score", json_object_new_double(affinities[0].score));
            json_object_object_add(w, "shape", json_object_new_string(mb_confidence_shape_name(shape)));
            json_object_object_add(w, "singleClause", json_object_new_boolean(single));
            json_object_object_add(w, "decision", json_object_new_string(sk_decision_name(sk_registry_decide(&d->skills, winner->app, winner->skill))));
            bool dispatchable = shape != MB_SHAPE_NONE && single && sk_registry_decide(&d->skills, winner->app, winner->skill) == SK_ALLOWED;
            json_object_object_add(w, "dispatchable", json_object_new_boolean(dispatchable));
            if (shape != MB_SHAPE_NONE) {
                char stages[400] = "";
                struct json_object *args = mb_confidence_arguments(skill, app, job->text, stages, sizeof stages);
                json_object_object_add(w, "args", args ? args : json_object_new_object());
                json_object_object_add(w, "stages", json_object_new_string(stages));
            }
            json_object_object_add(ev, "winner", w);
        }
    }
    pthread_mutex_unlock(&d->index_lock);
    free(vector);
    json_object_object_add(ev, "ok", json_object_new_boolean(rc == 0));
    if (rc) json_object_object_add(ev, "message", json_object_new_string(message[0] ? message : strerror(-rc)));
    mr_queue_push(&d->queue, ev);
    free(job->text);
    free(job);
    atomic_fetch_sub(&d->workers, 1);
    return NULL;
}

static void triage_job(mr_daemon *d, int gen, unsigned client, const char *text) {
    struct triage_job *job = calloc(1, sizeof *job);
    if (!job) return;
    job->d = d;
    job->gen = gen;
    job->client = client;
    job->text = strdup(text);
    spawn(d, triage_worker, job);
}

/* ---- the turn ---- */

static void finish_turn(mr_daemon *d, bool cancelled);
static void continue_turn(mr_daemon *d, struct json_object *triage);
static void listen_now(mr_daemon *d, bool follow_up);

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
        size_t n = d->aops->play(d->audio, samples, count);
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

static void on_turn_end(bool completed, struct json_object *end, void *user) {
    struct json_object *o = typed("turn.end"), *v;
    json_object_object_add(o, "completed", json_object_new_boolean(completed));
    /* Gita's contribution and what was retrieved ride along to the desktop and the Thread. */
    if (end && json_object_object_get_ex(end, "contribution", &v)) json_object_object_add(o, "contribution", json_object_get(v));
    if (end && json_object_object_get_ex(end, "retrieved", &v)) json_object_object_add(o, "retrieved", json_object_get(v));
    post_turn(user, o);
}

static void on_tts_failed(long status, const char *message, void *user) {
    struct json_object *o = typed("turn.tts_failed");
    json_object_object_add(o, "status", json_object_new_int64(status));
    json_object_object_add(o, "message", json_object_new_string(message));
    post_turn(user, o);
}

static const mr_turn_events turn_events = { .token = on_token, .audio = on_audio, .tts_failed = on_tts_failed,
                                            .error = on_turn_error, .end = on_turn_end };
static struct turn_ctx *turn_ctx;       /* the running turn's; one turn at a time */

static struct json_object *str_or_empty(const char *s) { return json_object_new_string(s ? s : ""); }

/* The episode's record for the Thread: the behaviour, one JSON line in the Mac's codec, tagged as the Mac tags it
 * (ThreadMemoryTopology, BehavioralAssembler.flush). The interaction stub the Mac wrote beside it was retired:
 * the turn's id is in the metadata, and there is no conversation record for it to join. */
static struct json_object *behavior_items(mr_daemon *d) {
    const mf_behavior_episode *e = &d->episode;
    char *line = mf_behavior_encode(e);
    if (!line) return NULL;
    struct json_object *items = json_object_new_array();
    char id[96], group[96], name[128];
    snprintf(id, sizeof id, "mary-behavior-%s", e->id);
    if (e->target_count) sk_ability_group(d->owner, e->targets[0].ability_id, e->targets[0].paradigm, group, sizeof group);
    else snprintf(group, sizeof group, "mary-behavior-%s", d->owner);
    snprintf(name, sizeof name, "Behaviour: %.80s", e->query);
    struct json_object *b = json_object_new_object();
    json_object_object_add(b, "type", json_object_new_string("deposit"));
    json_object_object_add(b, "source", json_object_new_string("maryd"));
    json_object_object_add(b, "document_id", json_object_new_string(id));
    json_object_object_add(b, "group", json_object_new_string(group));
    char label[160];
    if (e->target_count) snprintf(label, sizeof label, "Behaviour \xE2\x80\x94 %s", e->targets[0].ability_id);
    else snprintf(label, sizeof label, "Mary \xC2\xB7 behaviour");
    json_object_object_add(b, "label", json_object_new_string(label));
    json_object_object_add(b, "family", json_object_new_string("behavior"));
    json_object_object_add(b, "name", json_object_new_string(name));
    struct json_object *texts = json_object_new_array();
    json_object_array_add(texts, json_object_new_string(line));
    json_object_object_add(b, "texts", texts);
    struct json_object *tags = json_object_new_array();
    char tag[160];
    json_object_array_add(tags, json_object_new_string("schema:" MF_BEHAVIOR_SCHEMA));
    snprintf(tag, sizeof tag, "episode:%s", e->id);
    json_object_array_add(tags, json_object_new_string(tag));
    for (int i = 0; i < e->target_count; i++) {
        snprintf(tag, sizeof tag, "ability:%s", e->targets[i].ability_id);
        json_object_array_add(tags, json_object_new_string(tag));
    }
    for (int i = 0; i < e->action_count; i++) {
        snprintf(tag, sizeof tag, "skill:%s", e->actions[i].skill.invocation);
        json_object_array_add(tags, json_object_new_string(tag));
    }
    json_object_array_add(tags, json_object_new_string(mf_behavior_did_act(e) ? "did_act:true" : "did_act:false"));
    json_object_array_add(tags, json_object_new_string(e->target_count ? "thread:ability" : "thread:personal"));
    json_object_object_add(b, "tags", tags);
    struct json_object *meta = json_object_new_object();
    json_object_object_add(meta, "family", json_object_new_string("behavior"));
    json_object_object_add(meta, "schema", json_object_new_string(MF_BEHAVIOR_SCHEMA));
    json_object_object_add(meta, "schemaVersion", json_object_new_int(MF_BEHAVIOR_SCHEMA_VERSION));
    json_object_object_add(meta, "episode", json_object_new_string(e->id));
    json_object_object_add(meta, "turn", json_object_new_string(d->turn.request_id));
    json_object_object_add(meta, "lane", json_object_new_string(e->lane));
    json_object_object_add(meta, "intent", json_object_new_string(ma_intent_name(d->turn.intent)));
    json_object_object_add(meta, "did_act", json_object_new_boolean(mf_behavior_did_act(e)));
    json_object_object_add(meta, "actions", json_object_new_int(e->action_count));
    json_object_object_add(b, "metadata", meta);
    json_object_array_add(items, b);
    free(line);
    return items;
}

/* The turn is over, however it ran: the exchange joins the history and the Thread, the episode is sealed, and the
 * desktop hears reply.end. */
static void finish_turn(mr_daemon *d, bool cancelled) {
    if (!d->turn.open) return;
    d->turn.open = false;
    d->turn.triaging = false;
    const char *reply = d->turn.reply.data ? (char *)d->turn.reply.data : "";
    if (*reply) mb_history_append(&d->history, MB_ROLE_ASSISTANT, reply);
    if (d->episode_open) {
        mf_behavior_seal(&d->episode, cancelled ? MF_SEAL_CANCELLED : MF_SEAL_COMPLETED, wall_seconds());
        if (strcmp(d->episode.lane, "voice") != 0) deposit_items(d, behavior_items(d), "behaviour");
        snprintf(d->prior_episode_id, sizeof d->prior_episode_id, "%s", d->episode.id);
        mf_behavior_free(&d->episode);
        d->episode_open = false;
    }
    struct json_object *o = typed("reply.end");
    json_object_object_add(o, "cancelled", json_object_new_boolean(cancelled));
    if (d->turn.contribution) json_object_object_add(o, "contribution", json_object_get(d->turn.contribution));
    if (d->turn.retrieved) json_object_object_add(o, "retrieved", json_object_get(d->turn.retrieved));
    if (d->turn.runs) json_object_object_add(o, "runs", json_object_get(d->turn.runs));
    broadcast(d, o);
    if (d->turn.contribution) json_object_put(d->turn.contribution);
    if (d->turn.retrieved) json_object_put(d->turn.retrieved);
    d->turn.contribution = d->turn.retrieved = NULL;
}

/* Whatever the turn is doing stops: the sewnd worker, the lane, the triage in flight. */
static void close_turn(mr_daemon *d, bool cancelled) {
    if (!d->turn.worker && !d->turn.open && !d->bridge) return;
    if (d->turn.worker) {
        if (cancelled) {
            atomic_store(&d->turn_stopping, true);
            mr_turn_cancel(d->turn.worker);
            if (d->audio) d->aops->stop(d->audio);
        }
        mr_turn_free(d->turn.worker);
        atomic_store(&d->turn_stopping, false);
        d->turn.worker = NULL;
        free(turn_ctx);
        turn_ctx = NULL;
    }
    release_lane(d);
    atomic_store(&d->turn_gen, ++d->turn.gen);     /* whatever a worker still posts is stale */
    finish_turn(d, cancelled);
}

static void reset_turn(mr_daemon *d) {
    free(d->turn.question);
    d->turn.question = NULL;
    mc_buf_free(&d->turn.reply);
    if (d->turn.start) json_object_put(d->turn.start);
    if (d->turn.runs) json_object_put(d->turn.runs);
    if (d->turn.scope) json_object_put(d->turn.scope);
    free(d->turn.system_prompt);
    d->turn.start = d->turn.runs = d->turn.scope = NULL;
    d->turn.system_prompt = NULL;
    d->turn.spoken_reply = d->turn.action_turn = d->turn.implies_action = false;
    d->turn.open = d->turn.triaging = false;
    d->turn.lane[0] = d->turn.lead_name[0] = 0;
    d->turn.lane_count = 0;
    d->turn.failed = d->turn.audio_started = d->turn.draining = d->turn.speaker_reported = false;
    d->turn.quiet_since = d->turn.audio_started_ms = 0;
}

/* ---- a voice sample: one text spoken through the speaker, kept out of the conversation and Thread ---- */

struct sample_ctx {
    mr_daemon *d;
    int gen;
    bool heard_audio;
};

static struct sample_ctx *sample_ctx;       /* the running sample's */

static void post_sample(struct sample_ctx *s, struct json_object *event) {
    json_object_object_add(event, "gen", json_object_new_int(s->gen));
    mr_queue_push(&s->d->queue, event);
}

static void on_sample_audio(const float *samples, size_t count, void *user) {
    struct sample_ctx *s = user;
    if (!s->heard_audio) {
        s->heard_audio = true;
        post_sample(s, typed("sample.audio"));
    }
    mr_daemon *d = s->d;
    while (d->audio && count && atomic_load(&d->sample_gen) == s->gen) {
        size_t n = d->aops->play(d->audio, samples, count);
        samples += n;
        count -= n;
        if (!n) usleep(10000);
    }
}

static void on_sample_failed(long status, const char *message, void *user) {
    struct json_object *o = typed("sample.failed");
    json_object_object_add(o, "message", json_object_new_string(message));
    post_sample(user, o);
}

static void on_sample_error(const char *stage, const char *message, void *user) { on_sample_failed(0, message, user); }

static void on_sample_end(bool completed, struct json_object *end, void *user) { post_sample(user, typed("sample.end")); }

static const mr_turn_events sample_events = { .audio = on_sample_audio, .tts_failed = on_sample_failed,
                                              .error = on_sample_error, .end = on_sample_end };

/* voice.sample{voice_id, state, message?}, to the client that asked. */
static void sample_state(mr_daemon *d, const char *state, const char *message) {
    mr_client *c = mr_desktop_client(&d->desktop, d->sample.client);
    if (!c) return;
    struct json_object *o = typed("voice.sample");
    json_object_object_add(o, "voice_id", json_object_new_string(d->sample.voice_id));
    json_object_object_add(o, "state", json_object_new_string(state));
    if (message) json_object_object_add(o, "message", json_object_new_string(message));
    send_to(d, c, o);
}

static void close_sample(mr_daemon *d, bool cancelled) {
    if (!d->sample.worker) return;
    atomic_store(&d->sample_gen, ++d->sample.gen);  /* the audio callback stops waiting for room */
    if (cancelled) {
        mr_turn_cancel(d->sample.worker);
        if (d->audio) d->aops->stop(d->audio);
    }
    mr_turn_free(d->sample.worker);
    d->sample.worker = NULL;
    free(sample_ctx);
    sample_ctx = NULL;
}

/* A question, listening or Stop ends a sample: whoever asked for it hears it is done. */
static void stop_sample(mr_daemon *d) {
    bool active = d->sample.worker || d->sample.draining;
    close_sample(d, true);
    if (d->sample.draining && d->audio) d->aops->stop(d->audio);
    d->sample.draining = false;
    if (active) {
        sample_state(d, "done", NULL);
        standby(d);
    }
}

static void start_sample(mr_daemon *d, mr_client *c, struct json_object *msg) {
    const char *voice = mc_json_string(msg, "voice_id"), *text = mc_json_string(msg, "text");
    stop_sample(d);
    d->sample.client = c->id;
    snprintf(d->sample.voice_id, sizeof d->sample.voice_id, "%s", sewn_voice_id_valid(voice) ? voice : "");
    if (!d->sample.voice_id[0]) {
        sample_state(d, "failed", "That is not a voice.");
        return;
    }
    if (d->turn.worker || d->turn.draining || (d->state != MV_STATE_IDLE && d->state != MV_STATE_ERROR)) {
        sample_state(d, "failed", "Mary is busy; try again when she is quiet.");
        return;
    }
    if (!text || !*text) text = "Hello! This is how I will sound.";
    if (strlen(text) > SEWN_SPEAK_TEXT_MAX) {
        sample_state(d, "failed", "That sample is too long.");
        return;
    }
    struct json_object *start = typed("speak");
    json_object_object_add(start, "voice_id", json_object_new_string(d->sample.voice_id));
    json_object_object_add(start, "text", json_object_new_string(text));
    mr_ears_mute(d->ears);                          /* her own voice must not wake her */
    microphone(d, false);
    d->sample.failed = d->sample.audio_started = false;
    sample_ctx = calloc(1, sizeof *sample_ctx);
    int error = 0;
    if (sample_ctx) {
        sample_ctx->d = d;
        sample_ctx->gen = d->sample.gen;
        d->sample.worker = mr_turn_start(d->sewn_path, start, &sample_events, sample_ctx, &error);
    }
    json_object_put(start);
    if (!d->sample.worker) {
        free(sample_ctx);
        sample_ctx = NULL;
        standby(d);
        sample_state(d, "failed", error == -ENOENT || error == -ECONNREFUSED ? "sewnd is not running" : strerror(-error));
        return;
    }
    sample_state(d, "asking", NULL);
}

static void on_sample_event(mr_daemon *d, const char *type, struct json_object *ev) {
    int64_t gen = -1;
    if (!mc_json_int64(ev, "gen", &gen) || gen != d->sample.gen || !d->sample.worker) return;
    if (strcmp(type, "sample.audio") == 0) {
        if (!d->audio && d->config.audio) {
            if (!d->sample.failed) sample_state(d, "failed", "Mary has no speaker: PipeWire could not be opened.");
            d->sample.failed = true;
            return;
        }
        d->sample.audio_started = true;
        d->sample.audio_started_ms = mc_now_ms();
        sample_state(d, "playing", NULL);
    } else if (strcmp(type, "sample.failed") == 0) {
        if (!d->sample.failed) sample_state(d, "failed", mc_json_string(ev, "message"));
        d->sample.failed = true;
    } else if (strcmp(type, "sample.end") == 0) {
        close_sample(d, false);
        if (d->sample.failed) standby(d);
        else d->sample.draining = true;             /* done once the speaker is quiet (tick) */
    }
}

/* The surfaces the prompt renders: every fresh one, the lead's first. */
static int lead_first_surfaces(mr_daemon *d, const ma_place *lead, double now, const ma_surface **out, int max) {
    int n = ma_store_surfaces(d->ambient, now, out, max);
    for (int i = 1; lead && i < n; i++) {
        if (!ma_place_equal(&out[i]->place, lead)) continue;
        const ma_surface *s = out[i];
        memmove(&out[1], &out[0], (size_t)i * sizeof *out);
        out[0] = s;
        break;
    }
    return n;
}

/* Lane A: the voice, with retrieval, through sewnd's turn.start. */
static void start_voice(mr_daemon *d) {
    turn_ctx = calloc(1, sizeof *turn_ctx);
    int error = 0;
    if (turn_ctx) {
        turn_ctx->d = d;
        turn_ctx->gen = d->turn.gen;
        d->turn.worker = mr_turn_start(d->sewn_path, d->turn.start, &turn_events, turn_ctx, &error);
    }
    if (!d->turn.worker) {
        free(turn_ctx);
        turn_ctx = NULL;
        broadcast(d, error_message("sewnd", error == -ENOENT || error == -ECONNREFUSED ? "sewnd is not running" : strerror(-error)));
        d->turn.failed = true;
        finish_turn(d, false);
        set_state(d, MV_STATE_ERROR);
        standby(d);
        return;
    }
    set_state(d, MV_STATE_THINKING);
}

/* The reply of a lane that has no voice of its own (Lane B's answer, a dispatch's receipt): shown at once, spoken
 * through sewnd's speak, and the turn ends when the speaker is quiet. */
static void finish_with_text(mr_daemon *d, const char *text) {
    if (!d->turn.open) return;
    if (text && *text) {
        mc_buf_append_str(&d->turn.reply, text);
        struct json_object *o = typed("reply.delta");
        json_object_object_add(o, "text", json_object_new_string(text));
        broadcast(d, o);
    }
    if (!text || !*text || !d->config.audio) {
        finish_turn(d, false);
        if (d->turn.voice) listen_now(d, true);
        else { standby(d); set_state(d, MV_STATE_IDLE); }
        return;
    }
    char spoken[SEWN_SPEAK_TEXT_MAX + 1];
    snprintf(spoken, sizeof spoken, "%s", text);
    if (strlen(text) > SEWN_SPEAK_TEXT_MAX) {          /* speak whole sentences, up to the limit */
        char *cut = spoken + SEWN_SPEAK_TEXT_MAX - 1;
        while (cut > spoken && !(*cut == '.' || *cut == '!' || *cut == '?')) cut--;
        if (cut > spoken) cut[1] = 0;
    }
    struct json_object *speak = typed("speak");
    json_object_object_add(speak, "voice_id", json_object_new_string(d->voice_id));
    json_object_object_add(speak, "text", json_object_new_string(spoken));
    turn_ctx = calloc(1, sizeof *turn_ctx);
    int error = 0;
    if (turn_ctx) {
        turn_ctx->d = d;
        turn_ctx->gen = d->turn.gen;
        d->turn.worker = mr_turn_start(d->sewn_path, speak, &turn_events, turn_ctx, &error);
    }
    json_object_put(speak);
    if (!d->turn.worker) {
        free(turn_ctx);
        turn_ctx = NULL;
        finish_turn(d, false);
        if (d->turn.voice) listen_now(d, true);
        else { standby(d); set_state(d, MV_STATE_IDLE); }
        return;
    }
    d->turn.spoken_reply = true;
}

/* One skill run for the trace, the reply's chips and the episode (BehavioralAssembler.capture). */
static void note_run(mr_daemon *d, struct json_object *outcome, bool from_lane) {
    const char *app = mc_json_string(outcome, "app"), *skill = mc_json_string(outcome, "skill"), *summary = mc_json_string(outcome, "summary");
    bool ok = false, requested = false, found_nothing = false, is_read = false;
    mc_json_bool(outcome, "ok", &ok);
    mc_json_bool(outcome, "requested", &requested);
    mc_json_bool(outcome, "found_nothing", &found_nothing);
    mc_json_bool(outcome, "is_read", &is_read);
    double started = 0, finished = 0;
    mc_json_double(outcome, "started", &started);
    mc_json_double(outcome, "finished", &finished);
    const sk_skill *s = sk_registry_skill(&d->skills, app, skill);
    ma_trace_note_skill_result(d->trace, d->turn.request_id, app, skill, requested ? "blocked" : ok ? "completed" : "failed", found_nothing, summary, finished ? finished : wall_seconds());
    if (d->turn.runs) json_object_array_add(d->turn.runs, json_object_get(outcome));
    if (d->episode_open) {
        mf_action_record r;
        memset(&r, 0, sizeof r);
        snprintf(r.id, sizeof r.id, "%s", mc_json_string(outcome, "call_id") ? mc_json_string(outcome, "call_id") : "");
        snprintf(r.intention, sizeof r.intention, "%s", mc_json_string(outcome, "invocation") ? mc_json_string(outcome, "invocation") : "");
        snprintf(r.arguments_json, sizeof r.arguments_json, "%s", mc_json_string(outcome, "args") ? mc_json_string(outcome, "args") : "{}");
        snprintf(r.skill.package_id, sizeof r.skill.package_id, "%s", app ? app : "");
        snprintf(r.skill.ability_id, sizeof r.skill.ability_id, "%s", app ? app : "");
        snprintf(r.skill.skill_id, sizeof r.skill.skill_id, "%s", skill ? skill : "");
        snprintf(r.skill.invocation, sizeof r.skill.invocation, "%s", r.intention);
        snprintf(r.skill.version, sizeof r.skill.version, "1");
        snprintf(r.adapters[0], sizeof r.adapters[0], "desktop");
        r.adapter_count = 1;
        r.disposition = requested ? MF_DISPOSITION_REQUESTED_CONFIRMATION
                      : ok ? MF_DISPOSITION_SUCCEEDED
                      : summary && strstr(summary, "turned off") ? MF_DISPOSITION_BLOCKED : MF_DISPOSITION_FAILED;
        snprintf(r.summary, sizeof r.summary, "%s", summary ? summary : "");
        r.found_nothing = found_nothing;
        r.undoable = s && s->access == SK_ACCESS_REVERSIBLE;
        r.initiator = from_lane ? MF_INITIATOR_MODEL : is_read ? MF_INITIATOR_MARY_READ : MF_INITIATOR_MARY_ACT;
        mf_behavior_append(&d->episode, &r);
        if (app) mf_behavior_add_target(&d->episode, app, "applicationExpertise");
    }
}

/* Lane B: the silent skills loop on a thread of its own, bridged to this loop. */
static void start_lane(mr_daemon *d) {
    struct lane_bridge *b = calloc(1, sizeof *b);
    if (!b) { finish_with_text(d, "I could not begin."); return; }
    b->d = d;
    b->gen = d->turn.gen;
    pthread_mutex_init(&b->lock, NULL);
    pthread_cond_init(&b->cond, NULL);
    atomic_init(&b->cancelled, false);
    b->messages = mb_history_spoken_messages(&d->history);
    b->scope = d->turn.scope ? json_object_get(d->turn.scope) : NULL;
    b->system = strdup(d->turn.system_prompt ? d->turn.system_prompt : "");
    sk_registry_init(&b->registry);
    if (d->skills_message) sk_registry_load(&b->registry, d->skills_message);
    b->implies_action = d->turn.implies_action;
    snprintf(b->request_id, sizeof b->request_id, "%s", d->turn.request_id);
    snprintf(b->provider, sizeof b->provider, "%s", d->skill_engine);
    d->bridge = b;
    snprintf(d->turn.lane, sizeof d->turn.lane, "orchestrator");
    snprintf(d->episode.lane, sizeof d->episode.lane, "orchestrator");
    mc_log(MC_LOG_DEBUG, "turn %s: the skills lane", d->turn.request_id);
    spawn(d, lane_thread, b);
    set_state(d, MV_STATE_THINKING);
}

/* The embedding dispatch (TurnTriage's shortcut): one skill, no model round; the receipt is the reply, and the
 * words become a routing habit in the Thread. */
struct dispatch_call {
    mr_daemon *d;
    int gen;
    char app[64], skill[64], invocation[128];
    char *args;
    double started;
};

static void on_dispatch_done(const mcu_result *r, void *user) {
    struct dispatch_call *call = user;
    mr_daemon *d = call->d;
    if (call->gen == d->turn.gen && d->turn.open) {
        struct json_object *outcome = json_object_new_object();
        json_object_object_add(outcome, "call_id", str_or_empty(r->call_id));
        json_object_object_add(outcome, "app", json_object_new_string(call->app));
        json_object_object_add(outcome, "skill", json_object_new_string(call->skill));
        json_object_object_add(outcome, "invocation", json_object_new_string(call->invocation));
        json_object_object_add(outcome, "args", json_object_new_string(call->args ? call->args : "{}"));
        json_object_object_add(outcome, "ok", json_object_new_boolean(r->ok));
        json_object_object_add(outcome, "requested", json_object_new_boolean(0));
        bool found_nothing = false, landed = false;
        const char *said = NULL;
        if (r->ok && r->result && json_object_is_type(r->result, json_type_object)) {
            mc_json_bool(r->result, "found_nothing", &found_nothing);
            mc_json_bool(r->result, "landed", &landed);
            said = mc_json_string(r->result, "summary");
            if (!said) said = mc_json_string(r->result, "message");
        }
        const sk_skill *s = sk_registry_skill(&d->skills, call->app, call->skill);
        json_object_object_add(outcome, "found_nothing", json_object_new_boolean(found_nothing));
        json_object_object_add(outcome, "is_read", json_object_new_boolean(s && s->effect == SK_EFFECT_READ));
        char summary[240];
        if (r->ok) snprintf(summary, sizeof summary, "%s", said && *said ? said : found_nothing ? "Nothing there." : "Done.");
        else snprintf(summary, sizeof summary, "That did not work: %s.", r->error ? r->error : "the app did not answer");
        json_object_object_add(outcome, "summary", json_object_new_string(summary));
        json_object_object_add(outcome, "started", json_object_new_double(call->started));
        json_object_object_add(outcome, "finished", json_object_new_double(wall_seconds()));
        note_run(d, outcome, false);
        json_object_put(outcome);
        finish_with_text(d, summary);
    }
    free(call->args);
    free(call);
}

static void dispatch_now(mr_daemon *d, struct json_object *winner) {
    const char *app = mc_json_string(winner, "app"), *skill = mc_json_string(winner, "skill"), *invocation = mc_json_string(winner, "invocation");
    struct json_object *args = mc_json_object(winner, "args");
    struct dispatch_call *call = calloc(1, sizeof *call);
    if (!call) { finish_with_text(d, "I could not begin."); return; }
    call->d = d;
    call->gen = d->turn.gen;
    snprintf(call->app, sizeof call->app, "%s", app);
    snprintf(call->skill, sizeof call->skill, "%s", skill);
    snprintf(call->invocation, sizeof call->invocation, "%s", invocation ? invocation : "");
    call->args = mf_canonical_json(args ? mc_json_compact(args, NULL) : "{}");
    call->started = wall_seconds();
    snprintf(d->turn.lane, sizeof d->turn.lane, "embedding");
    snprintf(d->episode.lane, sizeof d->episode.lane, "embedding");
    const sk_skill *s = sk_registry_skill(&d->skills, app, skill);
    ma_trace_note_skill_invocation(d->trace, d->turn.request_id, app, skill, s ? sk_effect_name(s->effect) : "", call->args, call->started);
    double score = 0;
    mc_json_double(winner, "score", &score);
    mc_log(MC_LOG_DEBUG, "turn %s: embedding dispatch %s (%.2f)", d->turn.request_id, invocation ? invocation : skill, score);
    /* the routing habit: these words led here (mary-routing-<intent>|<skill>|<epoch>) */
    struct json_object *items = json_object_new_array(), *habit = json_object_new_object();
    char id[200], group[96];
    snprintf(id, sizeof id, "mary-routing-%s|%s|%lld", ma_intent_name(d->turn.intent), invocation ? invocation : skill, (long long)d->turn.started_wall);
    snprintf(group, sizeof group, "mary-routing-%s", d->owner);
    json_object_object_add(habit, "type", json_object_new_string("deposit"));
    json_object_object_add(habit, "source", json_object_new_string("maryd"));
    json_object_object_add(habit, "document_id", json_object_new_string(id));
    json_object_object_add(habit, "group", json_object_new_string(group));
    json_object_object_add(habit, "label", json_object_new_string("Mary \xC2\xB7 how you ask"));
    json_object_object_add(habit, "family", json_object_new_string("routing"));
    json_object_object_add(habit, "name", json_object_new_string(d->turn.question));
    struct json_object *texts = json_object_new_array();
    json_object_array_add(texts, json_object_new_string(d->turn.question));
    json_object_object_add(habit, "texts", texts);
    struct json_object *meta = json_object_new_object();
    json_object_object_add(meta, "family", json_object_new_string("routing"));
    json_object_object_add(meta, "intent", json_object_new_string(ma_intent_name(d->turn.intent)));
    json_object_object_add(meta, "skill", json_object_new_string(invocation ? invocation : skill));
    json_object_object_add(meta, "app", json_object_new_string(app));
    json_object_object_add(meta, "score", json_object_new_double(score));
    json_object_object_add(habit, "metadata", meta);
    json_object_array_add(items, habit);
    deposit_items(d, items, "routing");
    if (mcu_invoke(d->pipes, app, skill, args, d->config.skill_timeout_ms, on_dispatch_done, call, NULL, 0) < 0) {
        free(call->args);
        free(call);
        ma_trace_note_skill_result(d->trace, d->turn.request_id, app, skill, "failed", false, "the desktop is not connected", wall_seconds());
        finish_with_text(d, "I could not reach the desktop.");
    }
}

/* Triage has spoken (or there was none): one lane runs (PORTING deviation 14). */
static void continue_turn(mr_daemon *d, struct json_object *triage) {
    d->turn.triaging = false;
    struct json_object *winner = triage ? mc_json_object(triage, "winner") : NULL;
    bool dispatchable = false;
    if (winner) mc_json_bool(winner, "dispatchable", &dispatchable);
    if (winner && dispatchable) dispatch_now(d, winner);
    else if (d->turn.action_turn || winner) start_lane(d);
    else start_voice(d);
}

/* The turn proper: the route, the prompt, the trace, and sewnd. */
static void begin_turn(mr_daemon *d, const char *question, bool voice) {
    d->turn.question = strdup(question);
    d->turn.voice = voice;
    d->turn.started_wall = mc_wall_ms();
    double now = wall_seconds();
    thread_turn_document_id(d->turn.started_wall, d->turn.request_id, sizeof d->turn.request_id);

    /* Resolve the turn once, before the prompt is assembled (AmbientEngine.resolve). */
    ma_focus_signal signal;
    ma_focus_project(&d->focus, now, &signal);
    ma_focus_evidence evidence[MA_FOCUS_LEDGER_MAX];
    int evidence_count = ma_focus_fresh_evidence(&d->focus, now, evidence, MA_FOCUS_LEDGER_MAX);
    ma_world world;
    bool has_world = ma_store_world(d->ambient, now, &world);
    ma_selection claimed;
    ma_store_claim_selection(d->ambient, now, &claimed);        /* frozen for this turn; the next turn needs a new capture */
    ma_route *route = malloc(sizeof *route);
    ma_rendering *rendering = malloc(sizeof *rendering);
    ma_fact *facts = malloc(72 * sizeof *facts);
    if (!route || !rendering || !facts) {
        free(route); free(rendering); free(facts);
        broadcast(d, error_message("turn", "out of memory"));
        return;
    }
    d->turn.open = true;
    d->turn.runs = json_object_new_array();
    snprintf(d->turn.lane, sizeof d->turn.lane, "voice");
    ma_engine_inputs inputs = {
        .utterance = question, .classify_edit = true, .bare_decision = -2, .world = has_world ? &world : NULL,
        .lead_application_id = signal.has_lead && ma_place_is_application(&signal.lead) ? signal.lead.application : NULL,
        .focus = &signal, .evidence = evidence, .evidence_count = evidence_count, .roster = &d->roster, .now = now,
    };
    ma_engine_resolve(&inputs, route);
    ma_store_note_utterance(d->ambient, question);
    ma_store_note_route(d->ambient, route);
    ma_place lead;
    bool has_lead = ma_route_lead_place(route, &lead);
    if (has_lead) ma_store_note_lead(d->ambient, &lead, now);

    /* Rank what she holds against the words, render it under the voice budget, and land it last in the instructions. */
    const ma_surface *surfaces[MA_RENDER_SURFACES];
    int surface_count = lead_first_surfaces(d, has_lead ? &lead : NULL, now, surfaces, MA_RENDER_SURFACES);
    int fact_count = ma_store_facts(d->ambient, now, facts, 72);
    const ma_world *routed = ma_route_routed_world(route);
    ma_render_inputs render_in = { .facts = facts, .fact_count = fact_count, .utterance = question, .focused = has_lead ? &lead : NULL,
                                   .world = routed, .surfaces = surfaces, .surface_count = surface_count, .budget = MA_VOICE_BUDGET,
                                   .now = now, .roster = &d->roster };
    ma_render(&render_in, rendering);
    ma_live_work_world live_world;
    if (surface_count && has_lead && ma_place_equal(&surfaces[0]->place, &lead)) ma_live_work_from_surface(surfaces[0], &d->roster, &live_world);
    else ma_live_work_from_world(routed, &d->roster, &live_world);
    mc_buf live = { 0 };
    ma_prompt_live_work(rendering, &live_world, route->selection_defines_turn, &live);
    char capability[512] = "";
    if (has_lead && route->intent != MA_INTENT_CONVERSE) ma_capability_line(&lead, &d->roster, capability, sizeof capability);

    mb_clock clock;
    mb_clock_now(&clock);
    mb_prompt_inputs prompt = { .conversational = route->intent == MA_INTENT_CONVERSE, .capability = capability[0] ? capability : NULL,
                                .live_work = live.len ? (const char *)live.data : NULL };
    char *instructions = mb_sewn_instructions_with(&clock, &prompt);

    /* The memory plan: which storage lanes each purpose may retrieve from (the Recall toggles applied), the groups
     * and the cues. The context purpose becomes the scope sewnd retrieves the reply with. */
    mb_memory_plan plan;
    mb_memory_plan_for(route, &d->recall, d->owner, &plan);
    d->turn.lane_count = plan.context.lane_count;
    for (int i = 0; i < plan.context.lane_count; i++) snprintf(d->turn.lanes[i], sizeof d->turn.lanes[i], "%s", plan.context.lanes[i]);
    mb_turn_request req = { .instructions = instructions, .owner_id = d->owner, .request_id = d->turn.request_id, .voice_id = d->voice_id, .provider = d->voice_engine,
                            .lanes = plan.context.lanes, .lane_count = plan.context.lane_count, .entities = plan.entities, .entity_count = plan.entity_count };
    struct json_object *start = mb_turn_start(&d->history, &req);
    d->turn.start = start;

    /* Lane B's prompt and scope, should the turn act (MaryBrain+Turn.swift runOrchestratorLane's inputs). */
    const ma_registration *lead_reg = has_lead && ma_place_is_application(&lead) ? ma_roster_registration(&d->roster, lead.application) : NULL;
    snprintf(d->turn.lead_name, sizeof d->turn.lead_name, "%s", lead_reg ? lead_reg->name : "");
    const char *lead_context[1] = { live.len ? (const char *)live.data : "" };
    mb_system_inputs system_in = { .registry = &d->skills, .lead_place_name = lead_reg ? lead_reg->name : NULL,
                                   .lead_context = lead_context, .lead_context_count = live.len ? 1 : 0 };
    d->turn.system_prompt = mb_system_prompt(&clock, &system_in);
    struct json_object *scope = json_object_new_object();
    json_object_object_add(scope, "owner_id", json_object_new_string(d->owner));
    json_object_object_add(scope, "request_id", json_object_new_string(d->turn.request_id));
    json_object_object_add(scope, "lanes", mb_purpose_json(&plan.orchestration));
    struct json_object *groups = json_object_new_array(), *cues = json_object_new_array();
    for (int i = 0; i < plan.group_count; i++) json_object_array_add(groups, json_object_new_string(plan.groups[i]));
    for (int i = 0; i < plan.entity_count; i++) json_object_array_add(cues, json_object_new_string(plan.entities[i]));
    json_object_object_add(scope, "groups", groups);
    json_object_object_add(scope, "entities", cues);
    d->turn.scope = scope;
    d->turn.intent = route->intent;
    d->turn.action_turn = ma_route_is_action_turn(route) || route->intent == MA_INTENT_REVISE || route->intent == MA_INTENT_DECIDE || route->intent == MA_INTENT_HALT;
    d->turn.implies_action = d->turn.action_turn || route->verdicts.action_turn;

    /* The episode opens with the turn (BehavioralAssembler.open): the query, the prior episode, the gate's targets. */
    char episode_id[MF_UUID_LEN + 1];
    mf_uuid_v4(episode_id);
    mf_behavior_open(&d->episode, episode_id, question, now, "mistral", "voice", MARY_VERSION);
    snprintf(d->episode.prior_episode_id, sizeof d->episode.prior_episode_id, "%s", d->prior_episode_id);
    for (int i = 0; i < route->gate.memory.target_count; i++)
        mf_behavior_add_target(&d->episode, route->gate.memory.targets[i].ability_id, ma_paradigm_name(route->gate.memory.targets[i].paradigm));
    d->episode_open = true;

    /* One row in the trace: what was decided, and what it cost. */
    ma_trace_record *record = calloc(1, sizeof *record);
    if (record) {
        snprintf(record->id, sizeof record->id, "%s", d->turn.request_id);
        record->date = now;
        snprintf(record->utterance, sizeof record->utterance, "%s", question);
        record->route = *route;
        record->system_prompt_chars = instructions ? (int)ma_utf8_count(instructions) : 0;
        for (int i = 0; i < d->roster.app_count && record->package_count < MA_TRACE_PACKAGES; i++)
            if (d->roster.apps[i].has_eyes) snprintf(record->packages[record->package_count++], MA_ID_MAX, "%s", d->roster.apps[i].id);
        record->co_active_count = signal.co_active_count;
        memcpy(record->co_active, signal.co_active, (size_t)signal.co_active_count * sizeof *signal.co_active);
        record->glanced_count = signal.glanced_count;
        memcpy(record->glanced, signal.glanced, (size_t)signal.glanced_count * sizeof *signal.glanced);
        ma_trace_push(d->trace, record);
        free(record);
    }
    mc_log(MC_LOG_DEBUG, "turn %s: %s via %s%s%s", d->turn.request_id, ma_intent_name(route->intent), ma_signal_name(route->decided_by),
           has_lead ? ", lead " : "", has_lead ? route->lead_application_id : "");
    free(instructions);
    mc_buf_free(&live);
    free(route);
    free(rendering);
    free(facts);
    set_state(d, MV_STATE_THINKING);

    /* Triage (TurnTriage): one embedding of the words against the skill index, on a worker; the answer, or its
     * absence, decides which lane runs. Without an index the turn goes straight on. */
    pthread_mutex_lock(&d->index_lock);
    bool indexed = d->index_ready;
    pthread_mutex_unlock(&d->index_lock);
    if (indexed) {
        d->turn.triaging = true;
        triage_job(d, d->turn.gen, 0, question);
    } else {
        continue_turn(d, NULL);
    }
}

static void cancel_pending(mr_daemon *d) {
    free(d->pending.question);
    d->pending.question = NULL;
    d->pending.active = false;
}

static void begin_pending(mr_daemon *d) {
    if (!d->pending.active) return;
    char *question = d->pending.question;
    bool voice = d->pending.voice;
    d->pending.question = NULL;
    d->pending.active = false;
    begin_turn(d, question, voice);
    free(question);
}

/* A turn begins by asking the desktop what is in front of the person (world.request); the answer, or 150 ms,
 * starts it (AmbientWorld.snapshot at the turn's entry). */
static void start_turn(mr_daemon *d, const char *question, bool voice) {
    struct json_object *said = typed("transcript");
    json_object_object_add(said, "text", json_object_new_string(question));
    json_object_object_add(said, "final", json_object_new_boolean(1));
    json_object_object_add(said, "source", json_object_new_string(voice ? "voice" : "typed"));
    /* The deterministic tier (DeterministicTier.decision): a parked confirmation and a bare yes or no need no turn
     * of their own — the lane that asked resumes. Anything else said to a card refuses it. */
    if (confirmation_pending(d)) {
        int decision = mb_deterministic_decision(question);
        if (decision >= 0) {
            mb_history_append(&d->history, MB_ROLE_USER, question);
            broadcast(d, said);
            answer_confirmation(d, NULL, decision == 1);
            set_state(d, MV_STATE_THINKING);
            return;
        }
        answer_confirmation(d, NULL, false);
    }
    stop_sample(d);
    cancel_pending(d);
    close_turn(d, true);
    reset_turn(d);
    mr_ears_mute(d->ears);
    microphone(d, false);
    mb_history_append(&d->history, MB_ROLE_USER, question);
    broadcast(d, said);
    set_state(d, MV_STATE_THINKING);

    mr_client *desktop = mr_desktop_the_desktop(&d->desktop);
    struct json_object *ask = typed("world.request");
    int sent = desktop ? mr_desktop_send(&d->desktop, desktop, ask) : -ENOTCONN;
    json_object_put(ask);
    if (sent == 0) {
        d->pending.active = true;
        d->pending.question = strdup(question);
        d->pending.voice = voice;
        d->pending.deadline = mc_now_ms() + 150;
        return;
    }
    begin_turn(d, question, voice);
}

static void stop_everything(mr_daemon *d) {
    cancel_pending(d);
    stop_sample(d);
    close_turn(d, true);
    if (d->audio) d->aops->stop(d->audio);
    d->turn.draining = false;
    standby(d);
    set_state(d, MV_STATE_IDLE);
}

/* What the reply's context asked for and what came back, on the turn's trace row (RetrievalTraceLedger). */
static void note_retrieval(mr_daemon *d) {
    ma_retrieval_purpose purpose;
    memset(&purpose, 0, sizeof purpose);
    snprintf(purpose.name, sizeof purpose.name, "context");
    purpose.lane_count = d->turn.lane_count;
    for (int i = 0; i < d->turn.lane_count; i++) snprintf(purpose.lanes[i], sizeof purpose.lanes[i], "%s", d->turn.lanes[i]);
    if (d->turn.retrieved && json_object_is_type(d->turn.retrieved, json_type_array)) {
        size_t n = json_object_array_length(d->turn.retrieved);
        for (size_t i = 0; i < n && purpose.returned_count < MA_TRACE_RETURNED; i++) {
            struct json_object *hit = json_object_array_get_idx(d->turn.retrieved, i);
            ma_retrieved *r = &purpose.returned[purpose.returned_count++];
            const char *id = mc_json_string(hit, "document_id"), *group = mc_json_string(hit, "group_id"), *family = mc_json_string(hit, "family"), *lane = mc_json_string(hit, "lane");
            snprintf(r->document_id, sizeof r->document_id, "%s", id ? id : "");
            snprintf(r->group_id, sizeof r->group_id, "%s", group ? group : "");
            snprintf(r->family, sizeof r->family, "%s", family ? family : "");
            snprintf(r->lane, sizeof r->lane, "%s", lane ? lane : "");
            mc_json_double(hit, "score", &r->score);
        }
    }
    if (!purpose.returned_count && purpose.lane_count) snprintf(purpose.warning, sizeof purpose.warning, "asked nothing back");
    ma_trace_note_retrieval(d->trace, d->turn.request_id, &purpose);
    if (d->turn.contribution) {
        struct json_object *owners = mc_json_array(d->turn.contribution, "owners");
        int owner_count = owners ? (int)json_object_array_length(owners) : 0, documents = 0, passages = 0;
        for (int i = 0; i < owner_count; i++) {
            struct json_object *owner = json_object_array_get_idx(owners, i), *ids = mc_json_array(owner, "document_ids"), *spans = mc_json_array(owner, "spans");
            documents += ids ? (int)json_object_array_length(ids) : 0;
            passages += spans ? (int)json_object_array_length(spans) : 0;
        }
        char summary[200];
        snprintf(summary, sizeof summary, "%d owner%s, %d document%s, %d passage%s", owner_count, owner_count == 1 ? "" : "s",
                 documents, documents == 1 ? "" : "s", passages, passages == 1 ? "" : "s");
        ma_trace_note_contribution(d->trace, d->turn.request_id, summary);
    }
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
        if (!d->audio && d->config.audio) {
            if (!d->turn.speaker_reported) broadcast(d, error_message("speaker", "Mary has no speaker: PipeWire could not be opened."));
            d->turn.speaker_reported = true;
            return;
        }
        d->turn.audio_started = true;
        d->turn.audio_started_ms = mc_now_ms();
        set_state(d, MV_STATE_SPEAKING);
    } else if (strcmp(type, "turn.tts_failed") == 0) {
        /* The words stand; say why they were not spoken. */
        const char *message = mc_json_string(ev, "message");
        mc_log(MC_LOG_WARNING, "the reply was not spoken: %s", message ? message : "");
        broadcast(d, error_message("speech", message ? message : "Mistral could not speak the reply"));
    } else if (strcmp(type, "turn.error") == 0) {
        d->turn.failed = true;
        broadcast(d, error_message(mc_json_string(ev, "stage"), mc_json_string(ev, "message")));
    } else if (strcmp(type, "turn.end") == 0) {
        bool completed = false;
        mc_json_bool(ev, "completed", &completed);
        bool failed = d->turn.failed && !d->turn.reply.len;
        struct json_object *v;
        if (json_object_object_get_ex(ev, "contribution", &v)) d->turn.contribution = json_object_get(v);
        if (json_object_object_get_ex(ev, "retrieved", &v)) d->turn.retrieved = json_object_get(v);
        if (!d->turn.spoken_reply) note_retrieval(d);
        close_turn(d, false);
        if (failed) {
            set_state(d, MV_STATE_ERROR);
            standby(d);
        } else {
            d->turn.draining = true;        /* tick waits for the speaker */
        }
    }
}

/* ---- Lane B's events, and triage's ---- */

struct lane_call {
    mr_daemon *d;
    int gen;
};

/* A skill the lane asked for came back (on this thread, through the pipes): wake the lane. */
static void on_lane_invoked(const mcu_result *r, void *user) {
    struct lane_call *call = user;
    struct lane_bridge *b = call->d->bridge;
    if (b && b->gen == call->gen) {
        pthread_mutex_lock(&b->lock);
        b->invoke_rc = r->ok ? 0 : -EIO;
        if (b->invoke_result) json_object_put(b->invoke_result);
        b->invoke_result = r->result ? json_object_get(r->result) : NULL;
        snprintf(b->invoke_error, sizeof b->invoke_error, "%s", r->error ? r->error : "");
        b->invoke_done = true;
        pthread_cond_broadcast(&b->cond);
        pthread_mutex_unlock(&b->lock);
    }
    free(call);
}

static void on_lane_event(mr_daemon *d, const char *type, struct json_object *ev) {
    int64_t gen = -1;
    mc_json_int64(ev, "gen", &gen);
    bool current = d->bridge && gen == d->bridge->gen && d->turn.open;
    if (strcmp(type, "lane.invoke") == 0) {
        if (!current) return;
        struct lane_call *call = malloc(sizeof *call);
        if (!call) return;
        *call = (struct lane_call){ d, (int)gen };
        if (mcu_invoke(d->pipes, mc_json_string(ev, "app"), mc_json_string(ev, "skill"), mc_json_object(ev, "args"), d->config.skill_timeout_ms, on_lane_invoked, call, NULL, 0) < 0) {
            mcu_result r = { .ok = false, .error = "disconnected" };
            on_lane_invoked(&r, call);
        }
    } else if (strcmp(type, "lane.confirm") == 0) {
        if (!current) return;
        struct json_object *card = typed("skill.confirm");
        const char *keys[] = { "call_id", "app", "app_name", "skill", "title", "summary" };
        for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) json_object_object_add(card, keys[i], str_or_empty(mc_json_string(ev, keys[i])));
        struct json_object *args = mc_json_object(ev, "args");
        json_object_object_add(card, "args", args ? json_object_get(args) : json_object_new_object());
        broadcast(d, card);
        ma_trace_note_confirmation(d->trace, d->turn.request_id, "pending");
    } else if (strcmp(type, "lane.run") == 0) {
        if (!current) return;
        const char *app = mc_json_string(ev, "app"), *skill = mc_json_string(ev, "skill");
        const sk_skill *s = sk_registry_skill(&d->skills, app, skill);
        ma_trace_note_skill_invocation(d->trace, d->turn.request_id, app, skill, s ? sk_effect_name(s->effect) : "", mc_json_string(ev, "args"), wall_seconds());
    } else if (strcmp(type, "lane.result") == 0) {
        if (!current) return;
        struct json_object *outcome = mc_json_object(ev, "outcome");
        if (outcome) note_run(d, outcome, true);
    } else if (strcmp(type, "lane.done") == 0) {
        int64_t handle = 0;
        mc_json_int64(ev, "bridge", &handle);
        struct lane_bridge *b = (struct lane_bridge *)(intptr_t)handle;
        if (b == d->bridge) d->bridge = NULL;
        bridge_free(b);
        if (!current) return;
        bool ok = false, cancelled = false;
        mc_json_bool(ev, "ok", &ok);
        mc_json_bool(ev, "cancelled", &cancelled);
        const char *text = mc_json_string(ev, "text"), *error = mc_json_string(ev, "error");
        int64_t rounds = 0;
        mc_json_int64(ev, "rounds", &rounds);
        mc_log(MC_LOG_DEBUG, "turn %s: the skills lane ended after %lld round%s%s%s", d->turn.request_id, (long long)rounds, rounds == 1 ? "" : "s",
               error && *error ? ": " : "", error && *error ? error : "");
        if (cancelled) return;
        char fallback[300] = "";
        if (!text || !*text) {
            /* it only acted: the last run's word stands in for the prose (the Mac's voice lane would speak the receipt) */
            size_t n = d->turn.runs ? json_object_array_length(d->turn.runs) : 0;
            if (n) {
                struct json_object *last = json_object_array_get_idx(d->turn.runs, n - 1);
                const char *summary = mc_json_string(last, "summary");
                bool run_ok = false, requested = false;
                mc_json_bool(last, "ok", &run_ok);
                mc_json_bool(last, "requested", &requested);
                if (requested) fallback[0] = 0;
                else snprintf(fallback, sizeof fallback, "%s", summary && *summary ? summary : run_ok ? "Done." : "That did not work.");
            } else if (!ok) {
                snprintf(fallback, sizeof fallback, "I could not reach the model%s%s.", error && *error ? ": " : "", error && *error ? error : "");
                d->turn.failed = true;
                broadcast(d, error_message("sewnd", error && *error ? error : "the skills lane failed"));
            }
            text = fallback;
        }
        finish_with_text(d, text);
    }
}

static void on_triage_done(mr_daemon *d, struct json_object *ev) {
    int64_t client = 0, gen = -1;
    mc_json_int64(ev, "client", &client);
    mc_json_int64(ev, "gen", &gen);
    if (client) {
        mr_client *c = mr_desktop_client(&d->desktop, (unsigned)client);
        if (!c) return;
        struct json_object *o = typed("triage.result");
        const char *keys[] = { "ok", "message", "text", "winner", "affinities" };
        for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
            struct json_object *v;
            if (json_object_object_get_ex(ev, keys[i], &v)) json_object_object_add(o, keys[i], json_object_get(v));
        }
        send_to(d, c, o);
        return;
    }
    if (gen != d->turn.gen || !d->turn.triaging || !d->turn.open) return;
    bool ok = false;
    mc_json_bool(ev, "ok", &ok);
    if (!ok) mc_log(MC_LOG_DEBUG, "turn %s: triage abstained: %s", d->turn.request_id, mc_json_string(ev, "message") ? mc_json_string(ev, "message") : "");
    continue_turn(d, ok ? ev : NULL);
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

/* ---- the ambient world: what the desktop publishes (PARITY D28) ---- */

/* world{places[{place, capturedAt, surface}], focus, windows[]}: every surface is noted, the document each shows
 * becomes its file fact, and the focused place takes the lead. */
static void on_world(mr_daemon *d, struct json_object *msg) {
    double now = wall_seconds();
    struct json_object *places = mc_json_array(msg, "places");
    size_t n = places ? json_object_array_length(places) : 0;
    ma_surface *surface = malloc(sizeof *surface);
    if (!surface) return;
    for (size_t i = 0; i < n; i++) {
        struct json_object *entry = json_object_array_get_idx(places, i);
        if (ma_surface_parse(entry, now, surface) < 0) continue;
        ma_store_note_surface(d->ambient, surface, now);
        ma_fact fact;
        if (ma_surface_document_fact(entry, surface, &fact)) ma_store_replace_perceived(d->ambient, &surface->place, &fact, 1, now);
        else ma_store_forget_perceived(d->ambient, &surface->place);
    }
    free(surface);
    const char *focus = mc_json_string(msg, "focus");
    ma_place lead;
    if (focus && ma_place_from_token(focus, &lead)) {
        ma_focus_note(&d->focus, &lead, MA_EVIDENCE_ACTIVATION, now);
        ma_store_note_lead(d->ambient, &lead, now);
        ma_world world;
        memset(&world, 0, sizeof world);
        world.sense = MA_SENSE_WORKSPACE;
        world.attention = lead.attention;
        if (ma_place_is_application(&lead)) snprintf(world.application_id, sizeof world.application_id, "%s", lead.application);
        const ma_surface *s = ma_store_surface(d->ambient, &lead, now);
        if (s && s->has_window) snprintf(world.subject, sizeof world.subject, "%s", s->window_title);
        world.captured_at = now;
        world.fresh_for = ma_sense_fresh_for(MA_SENSE_WORKSPACE);
        ma_store_note_world(d->ambient, &world, now);
    }
    struct json_object *activity = mc_json_array(msg, "activity");     /* places where real work just happened */
    for (size_t i = 0; activity && i < json_object_array_length(activity); i++) {
        const char *token = json_object_get_string(json_object_array_get_idx(activity, i));
        ma_place place;
        if (token && ma_place_from_token(token, &place)) ma_focus_note(&d->focus, &place, MA_EVIDENCE_ACTIVITY, now);
    }
    if (d->pending.active) begin_pending(d);
}

static void on_selection(mr_daemon *d, struct json_object *msg) {
    double now = wall_seconds();
    ma_selection *sel = malloc(sizeof *sel);
    if (!sel) return;
    if (ma_selection_parse(msg, now, sel) == 0) ma_store_record_selection(d->ambient, sel, now);
    free(sel);
}

struct app_state_call {
    mr_daemon *d;
    unsigned client;
};

static void on_app_state_done(const mcu_result *r, void *user) {
    struct app_state_call *call = user;
    mr_client *c = mr_desktop_client(&call->d->desktop, call->client);
    if (c) {
        struct json_object *o = typed("app.state.result");
        if (r->call_id) json_object_object_add(o, "call_id", json_object_new_string(r->call_id));
        json_object_object_add(o, "ok", json_object_new_boolean(r->ok));
        if (r->ok) json_object_object_add(o, "surface", r->result ? json_object_get(r->result) : NULL);
        else json_object_object_add(o, "error", json_object_new_string(r->error ? r->error : "failed"));
        send_to(call->d, c, o);
    }
    free(call);
}

/* ---- the desktop's messages ---- */

static void on_message(mr_desktop *desktop, mr_client *c, struct json_object *msg, void *user) {
    mr_daemon *d = user;
    const char *type = mc_json_type(msg);
    if (strcmp(type, "ask") == 0) {
        const char *text = mc_json_string(msg, "text");
        if (text && *text) start_turn(d, text, false);
    } else if (strcmp(type, "listen") == 0) {
        stop_sample(d);
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
        else {
            key_job(d, "key.set", false, key, len);
            if (d->voices) json_object_put(d->voices);  /* another key may have other voices */
            d->voices = NULL;
        }
        if (key) mc_secure_zero((char *)key, len);      /* json-c's copy of it */
    } else if (strcmp(type, "key.verify") == 0 || strcmp(type, "key.status") == 0) {
        key_job(d, type, false, NULL, 0);
    } else if (strcmp(type, "config") == 0) {
        bool wake = true;
        if (mc_json_bool(msg, "wake", &wake)) {
            mr_ears_set_wake(d->ears, wake);
            if (d->state == MV_STATE_IDLE) microphone(d, false);
        }
        const char *voice = mc_json_string(msg, "voice");
        if (voice && sewn_voice_id_valid(voice)) snprintf(d->voice_id, sizeof d->voice_id, "%s", voice);
        else if (voice) send_to(d, c, error_message("config", "that is not a voice"));
        struct json_object *recall = mc_json_object(msg, "recall");
        if (recall) {
            mc_json_bool(recall, "personal", &d->recall.personal);
            mc_json_bool(recall, "behavioral", &d->recall.behavioral);
        }
        /* the engines: mistral is served; tinker rides the wire to sewnd, which answers the engine error (PORTING 12) */
        const char *engines[2] = { mc_json_string(msg, "voice_engine"), mc_json_string(msg, "skill_engine") };
        char *fields[2] = { d->voice_engine, d->skill_engine };
        for (int i = 0; i < 2; i++) {
            if (!engines[i]) continue;
            if (strcmp(engines[i], "mistral") == 0 || strcmp(engines[i], "tinker") == 0) snprintf(fields[i], 16, "%s", engines[i]);
            else send_to(d, c, error_message("config", "that is not an engine: mistral or tinker"));
        }
    } else if (strcmp(type, "calls.list") == 0) {
        int64_t limit = 50;
        mc_json_int64(msg, "limit", &limit);
        calls_job(d, c, (int)(limit > 0 && limit < 500 ? limit : 50));
    } else if (strcmp(type, "voices.list") == 0) {
        if (d->voices && mc_now_ms() - d->voices_at < 10 * 60 * 1000) {
            struct json_object *o = typed("voices");
            json_object_object_add(o, "ok", json_object_new_boolean(1));
            json_object_object_add(o, "voices", json_object_get(d->voices));
            send_to(d, c, o);
        } else {
            voices_job(d, c);
        }
    } else if (strcmp(type, "voice.sample") == 0) {
        start_sample(d, c, msg);
    } else if (strcmp(type, "skills") == 0) {
        pthread_mutex_lock(&d->index_lock);
        int loaded = sk_registry_load(&d->skills, msg);
        if (loaded == 0) { d->index_ready = false; d->skills_gen++; }     /* the index is of the registry it was built from */
        int skills_gen = d->skills_gen;
        pthread_mutex_unlock(&d->index_lock);
        if (loaded < 0) {
            send_to(d, c, error_message("skills", "a skills message that could not be read"));
            return;
        }
        if (d->skills_message) json_object_put(d->skills_message);
        d->skills_message = json_object_get(msg);
        ma_roster_merge_skills(&d->roster, mc_json_array(msg, "apps"));
        for (int i = 0; i < MR_CLIENTS_MAX; i++) d->desktop.clients[i].desktop = false;
        c->desktop = true;
        /* the skill index triage scores against, and the Thread's style record per discipline (idempotent ids) */
        (void)skills_gen;
        d->index_tries = d->styles_tries = 0;
        d->index_retry_at = d->styles_retry_at = 0;
        build_index(d);
        deposit_items(d, sk_style_records(&d->skills, d->owner), "style");
    } else if (strcmp(type, "skill.result") == 0) {
        mcu_pipes_on_message(d->pipes, msg);
    } else if (strcmp(type, "skills.list") == 0) {
        struct json_object *o = typed("skills"), *apps = d->skills_message ? mc_json_array(d->skills_message, "apps") : NULL;
        json_object_object_add(o, "apps", apps ? json_object_get(apps) : json_object_new_array());
        send_to(d, c, o);
    } else if (strcmp(type, "skill.call") == 0) {
        skill_call(d, c, msg);
    } else if (strcmp(type, "skill.confirm.reply") == 0) {
        bool yes = false;
        mc_json_bool(msg, "yes", &yes);
        if (!answer_confirmation(d, mc_json_string(msg, "call_id"), yes)) send_to(d, c, error_message("skill.confirm", "nothing is waiting for an answer"));
    } else if (strcmp(type, "triage") == 0) {
        const char *text = mc_json_string(msg, "text");
        pthread_mutex_lock(&d->index_lock);
        bool indexed = d->index_ready;
        pthread_mutex_unlock(&d->index_lock);
        if (!text || !*text) send_to(d, c, error_message("triage", "say what to triage"));
        else if (!indexed) send_to(d, c, error_message("triage", "the skill index is not built: the desktop has not published its skills, or sewnd could not embed them"));
        else triage_job(d, -1, c->id, text);
    } else if (strcmp(type, "world") == 0) {
        on_world(d, msg);
    } else if (strcmp(type, "selection") == 0) {
        on_selection(d, msg);
    } else if (strcmp(type, "selection.clear") == 0) {
        const char *app = mc_json_string(msg, "applicationID");
        if (app) ma_store_clear_selection(d->ambient, app, wall_seconds());
    } else if (strcmp(type, "app.state.result") == 0) {
        mcu_pipes_on_message(d->pipes, msg);
    } else if (strcmp(type, "app.state") == 0) {
        const char *app = mc_json_string(msg, "app");
        struct app_state_call *call = malloc(sizeof *call);
        if (!call) return;
        *call = (struct app_state_call){ d, c->id };
        if (!app || mcu_app_state(d->pipes, app, on_app_state_done, call) < 0) {
            free(call);
            struct json_object *o = typed("app.state.result");
            json_object_object_add(o, "ok", json_object_new_boolean(0));
            json_object_object_add(o, "error", json_object_new_string(app ? "disconnected" : "unknown"));
            send_to(d, c, o);
        }
    } else if (strcmp(type, "ambient.state") == 0) {
        double now = wall_seconds();
        ma_focus_signal signal;
        ma_focus_project(&d->focus, now, &signal);
        struct json_object *o = typed("ambient");
        json_object_object_add(o, "state", ma_ambient_state_json(d->ambient, &d->roster, &signal, now));
        send_to(d, c, o);
    } else if (strcmp(type, "trace.list") == 0) {
        struct json_object *o = typed("trace");
        json_object_object_add(o, "records", ma_trace_json(d->trace, &d->roster, wall_seconds()));
        send_to(d, c, o);
    } else if (strcmp(type, "trace.report") == 0) {
        mc_buf report = { 0 };
        ma_trace_report(d->trace, &d->roster, wall_seconds(), &report);
        struct json_object *o = typed("trace.report");
        json_object_object_add(o, "text", json_object_new_string(report.data ? (const char *)report.data : ""));
        mc_buf_free(&report);
        send_to(d, c, o);
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
    json_object_object_add(hello, "voice", json_object_new_string(d->voice_id));
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
        bool was_present = d->key_present;
        mc_json_bool(reply, "present", &d->key_present);
        mr_desktop_broadcast(&d->desktop, reply);
        /* a key just arrived: the skills can be embedded now */
        pthread_mutex_lock(&d->index_lock);
        bool indexed = d->index_ready;
        pthread_mutex_unlock(&d->index_lock);
        if (d->key_present && !was_present && !indexed) { d->index_tries = 0; build_index(d); }
    } else if (type && strcmp(type, "error") == 0) {
        mr_desktop_broadcast(&d->desktop, reply);
    }
}

/* sewnd's list of voices, for the client that asked; a good one is kept for the next ten minutes. */
static void on_voices_reply(mr_daemon *d, struct json_object *ev) {
    int64_t id = 0;
    mc_json_int64(ev, "client", &id);
    struct json_object *reply = mc_json_object(ev, "reply");
    const char *failure = mc_json_string(ev, "failure"), *type = reply ? mc_json_type(reply) : NULL;
    struct json_object *voices = type && strcmp(type, "voices") == 0 ? mc_json_array(reply, "voices") : NULL;
    if (voices) {
        if (d->voices) json_object_put(d->voices);
        d->voices = json_object_get(voices);
        d->voices_at = mc_now_ms();
    }
    mr_client *c = mr_desktop_client(&d->desktop, (unsigned)id);
    if (!c) return;
    struct json_object *o = typed("voices");
    json_object_object_add(o, "ok", json_object_new_boolean(voices != NULL));
    if (voices) {
        json_object_object_add(o, "voices", json_object_get(voices));
    } else {
        const char *said = type && strcmp(type, "error") == 0 ? mc_json_string(reply, "message") : NULL;
        json_object_object_add(o, "message", json_object_new_string(failure ? failure : said ? said : "sewnd did not list the voices"));
    }
    send_to(d, c, o);
}

/* sewnd's calls ledger, to the client that asked (Settings › Mary › Network activity). */
static void on_calls_reply(mr_daemon *d, struct json_object *ev) {
    int64_t id = 0;
    mc_json_int64(ev, "client", &id);
    mr_client *c = mr_desktop_client(&d->desktop, (unsigned)id);
    if (!c) return;
    struct json_object *reply = mc_json_object(ev, "reply");
    const char *failure = mc_json_string(ev, "failure"), *type = reply ? mc_json_type(reply) : NULL;
    struct json_object *calls = type && strcmp(type, "calls.list.result") == 0 ? mc_json_array(reply, "calls") : NULL;
    struct json_object *o = typed("calls");
    json_object_object_add(o, "ok", json_object_new_boolean(calls != NULL));
    if (calls) json_object_object_add(o, "calls", json_object_get(calls));
    else {
        const char *said = type && strcmp(type, "error") == 0 ? mc_json_string(reply, "message") : NULL;
        json_object_object_add(o, "message", json_object_new_string(failure ? failure : said ? said : "sewnd did not list its calls"));
    }
    send_to(d, c, o);
}

/* ---- the loop ---- */

static void on_capture(const int16_t *frame, size_t count, void *user) { mr_daemon_hear(user, frame, count); }

/* Opens the speaker and microphone, and opens them again once PipeWire has gone away (a restart, say), whenever
 * nothing is using them: no turn, no queued voice, Mary idle. A failure is retried after 1 s, doubling to 30 s. */
static void reopen_audio(mr_daemon *d, int64_t now) {
    if (!d->config.audio || d->turn.worker || d->turn.draining || d->sample.worker || d->sample.draining) return;
    if (d->audio && !d->aops->broken(d->audio)) return;
    if ((d->state != MV_STATE_IDLE && d->state != MV_STATE_ERROR) || now < d->audio_retry_at) return;
    bool again = d->audio != NULL;
    if (d->audio) {
        d->aops->close(d->audio);
        d->audio = NULL;
    }
    int error = 0;
    d->audio = d->aops->open(on_capture, d, &error, d->config.audio_user);
    if (d->audio) {
        if (again || d->audio_backoff_ms) mc_log(MC_LOG_INFO, "the speaker and microphone are open again");
        d->audio_backoff_ms = 0;
        d->audio_retry_at = 0;
        microphone(d, false);
        return;
    }
    if (!d->audio_backoff_ms) mc_log(MC_LOG_WARNING, "no microphone or speaker: %s; trying again", strerror(error < 0 ? -error : EIO));
    d->audio_backoff_ms = d->audio_backoff_ms ? (d->audio_backoff_ms >= 15000 ? 30000 : d->audio_backoff_ms * 2) : 1000;
    d->audio_retry_at = now + d->audio_backoff_ms;
}

static void tick(mr_daemon *d) {
    int64_t now = mc_now_ms();
    mcu_pipes_tick(d->pipes, now);
    if (d->pending.active && now >= d->pending.deadline) begin_pending(d);    /* the desktop said nothing in time */
    if (d->index_retry_at && now >= d->index_retry_at) { d->index_retry_at = 0; build_index(d); }
    if (d->styles_retry_at && now >= d->styles_retry_at) { d->styles_retry_at = 0; deposit_items(d, sk_style_records(&d->skills, d->owner), "style"); }
    reopen_audio(d, now);
    if (d->sample.draining) {
        int64_t played = d->audio ? d->aops->last_played_ms(d->audio) : 0;
        int64_t since = played > d->sample.audio_started_ms ? played : d->sample.audio_started_ms;
        if (d->audio && d->aops->queued(d->audio) > 0 && now - since >= d->config.speaker_stall_ms) {
            d->aops->stop(d->audio);
            d->sample.draining = false;
            sample_state(d, "failed", "Mary's voice is not reaching the speaker. Check Settings \xE2\x80\xBA Sound.");
            standby(d);
        } else if (!d->audio || d->aops->queued(d->audio) == 0) {
            d->sample.draining = false;
            sample_state(d, "done", NULL);
            standby(d);
        }
    }
    if (!d->turn.draining) return;
    if (d->audio && d->aops->queued(d->audio) > 0) {
        d->turn.quiet_since = 0;
        int64_t played = d->aops->last_played_ms(d->audio);
        int64_t since = played > d->turn.audio_started_ms ? played : d->turn.audio_started_ms;
        if (now - since < d->config.speaker_stall_ms) return;
        /* The speaker has stopped taking Mary's voice: drop the rest rather than stay speaking forever. */
        d->aops->stop(d->audio);
        mc_log(MC_LOG_WARNING, "the speaker took no audio for %d ms: the rest of the reply is dropped", d->config.speaker_stall_ms);
        broadcast(d, error_message("speaker", "Mary's voice is not reaching the speaker. Check Settings \xE2\x80\xBA Sound."));
        d->turn.audio_started = false;              /* nothing was heard: no echo to wait out */
    }
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
        if (d->config.speaker_stall_ms <= 0) d->config.speaker_stall_ms = 2000;
        d->aops = config->audio_ops ? config->audio_ops : &pipewire_ops;
        resolve(d->sewn_path, sizeof d->sewn_path, config->sewn_socket, "SEWN_SOCKET", SEWN_SOCKET_PATH);
        resolve(d->thread_local_path, sizeof d->thread_local_path, config->thread_local_socket, "THREAD_LOCAL_SOCKET", THREAD_LOCAL_SOCKET_PATH);
        pthread_mutex_init(&d->index_lock, NULL);
        d->recall = mb_recall_default();
        if (config->desktop_socket) snprintf(d->desktop_path, sizeof d->desktop_path, "%s", config->desktop_socket);
        else rc = mr_desktop_default_socket(d->desktop_path, sizeof d->desktop_path, true);
        mc_user_name(getuid(), d->owner, sizeof d->owner);
        atomic_init(&d->stop, false);
        atomic_init(&d->workers, 0);
        atomic_init(&d->turn_stopping, false);
        atomic_init(&d->turn_gen, 0);
        atomic_init(&d->sample_gen, 0);
        d->state = MV_STATE_IDLE;
        mb_history_init(&d->history);
        snprintf(d->voice_id, sizeof d->voice_id, "%s", MB_VOICE_ID);
        snprintf(d->voice_engine, sizeof d->voice_engine, "mistral");
        snprintf(d->skill_engine, sizeof d->skill_engine, "mistral");
        sk_registry_init(&d->skills);
        ma_roster_maryos(&d->roster);
        ma_focus_init(&d->focus);
        d->ambient = ma_store_new();
        d->trace = ma_trace_log_new(0);
        if (d->ambient) ma_store_set_roster(d->ambient, &d->roster);
        if (!d->ambient || !d->trace) rc = -ENOMEM;
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
    reopen_audio(d, mc_now_ms());
    key_job(d, "key.status", true, NULL, 0);
    return d;
}

int mr_daemon_run(mr_daemon *d) {
    struct pollfd fds[2 + MR_CLIENTS_MAX];
    while (!atomic_load(&d->stop)) {
        fds[0] = (struct pollfd){ .fd = mr_queue_fd(&d->queue), .events = POLLIN };
        size_t n = 1 + mr_desktop_pollfds(&d->desktop, fds + 1, sizeof fds / sizeof *fds - 1);
        int timeout = d->turn.draining || d->sample.draining || d->pending.active || mcu_pipes_pending(d->pipes) ? 50 : 1000;
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
            } else if (strncmp(type, "sample.", 7) == 0) {
                on_sample_event(d, type, ev);
            } else if (strncmp(type, "lane.", 5) == 0) {
                on_lane_event(d, type, ev);
            } else if (strcmp(type, "triage.done") == 0) {
                on_triage_done(d, ev);
            } else if (strcmp(type, "key.reply") == 0) {
                on_key_reply(d, ev);
            } else if (strcmp(type, "voices.reply") == 0) {
                on_voices_reply(d, ev);
            } else if (strcmp(type, "calls.reply") == 0) {
                on_calls_reply(d, ev);
            } else if (strcmp(type, "index.done") == 0) {
                bool ok = false, retryable = false;
                mc_json_bool(ev, "ok", &ok);
                mc_json_bool(ev, "retryable", &retryable);
                /* sewnd comes up beside the desktop, and a key may arrive later: try again, a while apart, for a few minutes */
                if (!ok && retryable && d->index_tries < 30) { d->index_tries++; d->index_retry_at = mc_now_ms() + (d->index_tries < 6 ? 5000 : 30000); }
                else d->index_retry_at = 0;
            } else if (strcmp(type, "deposit.done") == 0) {
                bool ok = false, unreachable = false;
                mc_json_bool(ev, "ok", &ok);
                mc_json_bool(ev, "unreachable", &unreachable);
                const char *what = mc_json_string(ev, "what");
                if (what && strcmp(what, "style") == 0) {
                    if (!ok && unreachable && d->styles_tries < 12) { d->styles_tries++; d->styles_retry_at = mc_now_ms() + 5000; }
                    else d->styles_retry_at = 0;
                }
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
    if (d->audio) d->aops->capture(d->audio, false);
    close_sample(d, true);
    close_turn(d, true);
    reset_turn(d);
    mr_ears_free(d->ears);
    if (d->audio) d->aops->close(d->audio);
    for (int i = 0; i < 600 && atomic_load(&d->workers) > 0; i++) usleep(10000);
    if (d->pipes) mcu_pipes_free(d->pipes);
    if (d->desktop.path[0]) mr_desktop_close(&d->desktop);
    if (atomic_load(&d->workers) == 0 && d->queue.items) mr_queue_free(&d->queue);
    if (d->skills_message) json_object_put(d->skills_message);
    if (d->voices) json_object_put(d->voices);
    sk_registry_free(&d->skills);
    mb_history_free(&d->history);
    cancel_pending(d);
    ma_store_free(d->ambient);
    ma_trace_log_free(d->trace);
    if (d->episode_open) mf_behavior_free(&d->episode);
    mb_skill_index_free(&d->skill_index);
    pthread_mutex_destroy(&d->index_lock);
    free(d);
}
