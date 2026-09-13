/* maryd end to end, with a fake sewnd, a fake threadd, and clients on the desktop
 * socket standing in for Spotlight and maryctl. Audio is injected where PipeWire's
 * capture would deliver it. */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "common/frame.h"
#include "common/io.h"
#include "common/json.h"
#include "common/lines.h"
#include "conduit/grpc.h"
#include "conduit/thread.pb-c.h"
#include "mary_test.h"
#include "runtime/daemon.h"
#include "sewn/client.h"

static char dir[64], sewn_sock[128], thread_sock[128], mary_sock[128];
static int sewn_listener = -1, thread_listener = -1;
static pthread_t sewn_thread, thread_thread, daemon_thread;
static mr_daemon *maryd;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int transcribes, pcm_bytes, index_calls;
static int last_turn_messages;
static bool last_turn_instructed;
static char last_user[128], indexed_question[128], indexed_reply[128], indexed_source[16];

/* ---- fake sewnd ---- */

static void send_json(int fd, const char *text) { mc_frame_write_fd(fd, MC_FRAME_JSON, (const unsigned char *)text, strlen(text)); }

static int on_upload(uint8_t kind, const unsigned char *payload, size_t len, void *user) {
    if (kind == MC_FRAME_PCM) atomic_fetch_add(&pcm_bytes, (int)len);
    if (kind != MC_FRAME_JSON) return 0;
    struct json_object *msg = mc_json_parse((const char *)payload, len);
    int end = msg && strcmp(mc_json_type(msg), "transcribe.end") == 0;
    json_object_put(msg);
    return end;
}

static void *sewn_connection(void *arg) {
    int fd = (int)(intptr_t)arg;
    struct json_object *first = NULL;
    if (sewn_read_reply(fd, &first) == 0) {
        const char *type = mc_json_type(first);
        if (strcmp(type, "turn.start") == 0) {
            struct json_object *request = mc_json_object(first, "request"), *messages = mc_json_array(request, "messages");
            pthread_mutex_lock(&lock);
            last_turn_messages = messages ? (int)json_object_array_length(messages) : 0;
            last_turn_instructed = mc_json_string(request, "instructions") != NULL;
            if (last_turn_messages)
                snprintf(last_user, sizeof last_user, "%s", mc_json_string(json_object_array_get_idx(messages, last_turn_messages - 1), "content"));
            pthread_mutex_unlock(&lock);
            send_json(fd, "{\"type\":\"phase\",\"phase\":\"grounded\"}");
            send_json(fd, "{\"type\":\"token\",\"phase\":\"grounded\",\"text\":\"Hello\"}");
            send_json(fd, "{\"type\":\"token\",\"phase\":\"grounded\",\"text\":\" there.\"}");
            send_json(fd, "{\"type\":\"audio.begin\",\"sample_rate\":24000,\"channels\":1,\"bits\":32,\"encoding\":\"f32le\"}");
            unsigned char pcm[480 * 4] = { 0 };
            mc_frame_write_fd(fd, MC_FRAME_PCM, pcm, sizeof pcm);
            send_json(fd, "{\"type\":\"turn.end\"}");
        } else if (strcmp(type, "transcribe.start") == 0) {
            atomic_fetch_add(&transcribes, 1);
            send_json(fd, "{\"type\":\"transcribe.ready\"}");
            mc_frame_reader reader;
            mc_frame_reader_init(&reader, 0, false);
            int rc;
            while ((rc = mc_frame_reader_read_fd(&reader, fd, on_upload, NULL)) == MC_IO_OK) {
            }
            mc_frame_reader_free(&reader);
            if (rc == MC_IO_STOPPED) {
                send_json(fd, "{\"type\":\"transcript.delta\",\"text\":\"what time\"}");
                send_json(fd, "{\"type\":\"transcript.done\",\"text\":\"what time is it\"}");
            }
        } else if (strcmp(type, "key.status") == 0) {
            send_json(fd, "{\"type\":\"key.status\",\"present\":true,\"verified_at\":1757700000000}");
        }
    }
    if (first) json_object_put(first);
    close(fd);
    return NULL;
}

static void *serve_sewn(void *arg) {
    for (;;) {
        int fd = accept(sewn_listener, NULL, NULL);
        if (fd < 0) return NULL;
        pthread_t t;
        pthread_create(&t, NULL, sewn_connection, (void *)(intptr_t)fd);
        pthread_detach(t);
    }
}

/* ---- fake threadd ---- */

static void on_index(const uint8_t *request, size_t len, conduit_reply *reply, void *user) {
    Thread__V1__ThreadIndexRequest *req = thread__v1__thread_index_request__unpack(NULL, len, request);
    if (req && req->n_items && req->items[0]->n_texts == 2) {
        struct json_object *meta = mc_json_parse((const char *)req->items[0]->metadata.data, req->items[0]->metadata.len);
        pthread_mutex_lock(&lock);
        snprintf(indexed_question, sizeof indexed_question, "%s", req->items[0]->texts[0]);
        snprintf(indexed_reply, sizeof indexed_reply, "%s", req->items[0]->texts[1]);
        snprintf(indexed_source, sizeof indexed_source, "%s", meta ? mc_json_string(meta, "source") : "?");
        pthread_mutex_unlock(&lock);
        json_object_put(meta);
        atomic_fetch_add(&index_calls, 1);
    }
    Thread__V1__ThreadIndexResponse resp = THREAD__V1__THREAD_INDEX_RESPONSE__INIT;
    resp.success = req != NULL;
    resp.indexed_count = req ? (int)req->n_items : 0;
    reply->body_len = thread__v1__thread_index_response__get_packed_size(&resp);
    reply->body = malloc(reply->body_len ? reply->body_len : 1);
    thread__v1__thread_index_response__pack(&resp, reply->body);
    if (req) thread__v1__thread_index_request__free_unpacked(req, NULL);
}

static const conduit_route thread_routes[] = { { "/thread.v1.ThreadQuery/Index", on_index } };

static void *serve_thread(void *arg) {
    for (;;) {
        int fd = accept(thread_listener, NULL, NULL);
        if (fd < 0) return NULL;
        conduit_serve(fd, thread_routes, 1, NULL);
        close(fd);
    }
}

/* ---- maryd and its clients ---- */

static void *run_daemon(void *arg) {
    mr_daemon_run(maryd);
    return NULL;
}

static void setup(void) {
    snprintf(dir, sizeof dir, "/tmp/maryd-XXXXXX");
    MARY_ASSERT(mkdtemp(dir) != NULL);
    snprintf(sewn_sock, sizeof sewn_sock, "%s/sewn.sock", dir);
    snprintf(thread_sock, sizeof thread_sock, "%s/thread.sock", dir);
    snprintf(mary_sock, sizeof mary_sock, "%s/mary.sock", dir);
    sewn_listener = mc_listen_unix(sewn_sock, 0600);
    thread_listener = mc_listen_unix(thread_sock, 0600);
    pthread_create(&sewn_thread, NULL, serve_sewn, NULL);
    pthread_create(&thread_thread, NULL, serve_thread, NULL);
    atomic_store(&index_calls, 0);
    atomic_store(&transcribes, 0);
    atomic_store(&pcm_bytes, 0);

    mr_config config = mr_config_default();
    config.desktop_socket = mary_sock;
    config.sewn_socket = sewn_sock;
    config.thread_socket = thread_sock;
    config.audio = false;
    config.wake = false;
    config.listen_timeout_ms = 400;
    config.echo_tail_ms = 0;
    config.skill_timeout_ms = 2000;
    int error = 0;
    maryd = mr_daemon_new(&config, &error);
    MARY_ASSERT(maryd != NULL);
    if (maryd) pthread_create(&daemon_thread, NULL, run_daemon, NULL);
}

static void teardown(void) {
    if (maryd) {
        mr_daemon_stop(maryd);
        pthread_join(daemon_thread, NULL);
        mr_daemon_free(maryd);
        maryd = NULL;
    }
    shutdown(sewn_listener, SHUT_RDWR);
    shutdown(thread_listener, SHUT_RDWR);
    pthread_join(sewn_thread, NULL);
    pthread_join(thread_thread, NULL);
    close(sewn_listener);
    close(thread_listener);
    unlink(sewn_sock);
    unlink(thread_sock);
    unlink(mary_sock);
    rmdir(dir);
}

typedef struct client {
    int fd;
    mc_line_reader lines;
    struct json_object *inbox[512];
    int count;
} client;

static int on_client_line(const char *line, size_t len, void *user) {
    client *c = user;
    struct json_object *msg = mc_json_parse(line, len);
    if (msg && c->count < 512) c->inbox[c->count++] = msg;
    else if (msg) json_object_put(msg);
    return 0;
}

static void client_open(client *c) {
    memset(c, 0, sizeof *c);
    c->fd = mc_connect_unix(mary_sock);
    MARY_ASSERT(c->fd >= 0);
    struct timeval patience = { .tv_sec = 5 };
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof patience);
    mc_line_reader_init(&c->lines, 1 << 20);
}

static void client_close(client *c) {
    for (int i = 0; i < c->count; i++) json_object_put(c->inbox[i]);
    c->count = 0;
    mc_line_reader_free(&c->lines);
    close(c->fd);
}

static void client_send(client *c, const char *line) {
    MARY_ASSERT_EQ(mc_write_all(c->fd, line, strlen(line)), 0);
    MARY_ASSERT_EQ(mc_write_all(c->fd, "\n", 1), 0);
}

/* The next message of `type` (with `state` when type is "state"), dropping those before
 * it into `seen` as "type" or "state:name" words. NULL after 5 s. The caller releases it. */
static struct json_object *client_wait(client *c, const char *type, const char *state, char *seen, size_t cap) {
    for (;;) {
        while (c->count) {
            struct json_object *msg = c->inbox[0];
            memmove(c->inbox, c->inbox + 1, (size_t)--c->count * sizeof *c->inbox);
            const char *t = mc_json_type(msg);
            if (seen && t) {
                size_t used = strlen(seen);
                if (strcmp(t, "state") == 0) snprintf(seen + used, cap - used, "state:%s ", mc_json_string(msg, "state"));
                else if (strcmp(t, "level") != 0) snprintf(seen + used, cap - used, "%s ", t);
            }
            if (t && strcmp(t, type) == 0 && (!state || strcmp(mc_json_string(msg, "state"), state) == 0)) return msg;
            json_object_put(msg);
        }
        char bytes[8192];
        ssize_t n = read(c->fd, bytes, sizeof bytes);
        if (n <= 0) return NULL;
        mc_line_reader_feed(&c->lines, bytes, (size_t)n, on_client_line, c);
    }
}

static bool eventually(atomic_int *counter, int want) {
    for (int i = 0; i < 300 && atomic_load(counter) < want; i++) usleep(10000);
    return atomic_load(counter) >= want;
}

MARY_TEST(a_typed_question_streams_a_reply_and_is_deposited_in_thread) {
    setup();
    client c;
    client_open(&c);
    struct json_object *hello = client_wait(&c, "hello", NULL, NULL, 0);
    MARY_ASSERT(hello != NULL);
    MARY_ASSERT_STR(mc_json_string(hello, "state"), "idle");
    json_object_put(hello);

    client_send(&c, "{\"type\":\"ask\",\"text\":\"Say hello\"}");
    char seen[1024] = "";
    struct json_object *end = client_wait(&c, "reply.end", NULL, seen, sizeof seen);
    MARY_ASSERT(end != NULL);
    bool cancelled = true;
    MARY_ASSERT(end && mc_json_bool(end, "cancelled", &cancelled) && !cancelled);
    json_object_put(end);
    /* Other broadcasts (key.status) may land in between; the order that matters is this. */
    const char *said = strstr(seen, "transcript "), *thinking = strstr(seen, "state:thinking "),
               *delta = strstr(seen, "reply.delta ");
    if (!(said && thinking && delta && said < thinking && thinking < delta && strstr(delta + 1, "reply.delta ") &&
          strstr(seen, "state:speaking ")))
        MARY_FAIL("the messages before reply.end were: %s", seen);
    struct json_object *idle = client_wait(&c, "state", "idle", NULL, 0);
    MARY_ASSERT(idle != NULL);
    json_object_put(idle);

    MARY_ASSERT(eventually(&index_calls, 1));
    pthread_mutex_lock(&lock);
    MARY_ASSERT_STR(indexed_question, "Say hello");
    MARY_ASSERT_STR(indexed_reply, "Hello there.");
    MARY_ASSERT_STR(indexed_source, "typed");
    MARY_ASSERT_EQ(last_turn_messages, 1);
    MARY_ASSERT(last_turn_instructed);
    pthread_mutex_unlock(&lock);

    client_send(&c, "{\"type\":\"ask\",\"text\":\"And again\"}");
    end = client_wait(&c, "reply.end", NULL, NULL, 0);
    MARY_ASSERT(end != NULL);
    json_object_put(end);
    pthread_mutex_lock(&lock);
    MARY_ASSERT_EQ(last_turn_messages, 3);          /* the first exchange rides along */
    MARY_ASSERT_STR(last_user, "And again");
    pthread_mutex_unlock(&lock);

    client late;
    client_open(&late);
    hello = client_wait(&late, "hello", NULL, NULL, 0);
    struct json_object *tail = hello ? mc_json_array(hello, "tail") : NULL;
    MARY_ASSERT(tail && json_object_array_length(tail) == 4);
    if (hello) json_object_put(hello);
    client_close(&late);
    client_close(&c);
    teardown();
}

MARY_TEST(skill_calls_go_through_the_desktops_pipes_and_its_policy) {
    setup();
    client desktop, ctl;
    client_open(&desktop);
    client_open(&ctl);
    json_object_put(client_wait(&desktop, "hello", NULL, NULL, 0));
    json_object_put(client_wait(&ctl, "hello", NULL, NULL, 0));
    client_send(&desktop,
        "{\"type\":\"skills\",\"apps\":["
        "{\"id\":\"settings\",\"name\":\"System Settings\",\"enabled\":true,\"ask\":\"never\",\"skills\":"
        "[{\"id\":\"open_pane\",\"title\":\"Open a pane\",\"summary\":\"\",\"params\":null,\"effect\":\"act\",\"enabled\":true}]},"
        "{\"id\":\"media\",\"name\":\"Media Player\",\"enabled\":false,\"ask\":\"changes\",\"skills\":"
        "[{\"id\":\"play_pause\",\"title\":\"Play or pause\",\"summary\":\"\",\"params\":null,\"effect\":\"act\",\"enabled\":true}]},"
        "{\"id\":\"calendar\",\"name\":\"Calendar\",\"enabled\":true,\"ask\":\"always\",\"skills\":"
        "[{\"id\":\"events_today\",\"title\":\"Today's events\",\"summary\":\"\",\"params\":null,\"effect\":\"read\",\"enabled\":true}]}]}");

    size_t apps = 0;
    for (int i = 0; i < 50 && apps != 3; i++) {
        client_send(&ctl, "{\"type\":\"skills.list\"}");
        struct json_object *list = client_wait(&ctl, "skills", NULL, NULL, 0);
        struct json_object *array = list ? mc_json_array(list, "apps") : NULL;
        apps = array ? json_object_array_length(array) : 0;
        if (list) json_object_put(list);
        if (apps != 3) usleep(20000);
    }
    MARY_ASSERT_EQ(apps, 3);

    client_send(&ctl, "{\"type\":\"skill.call\",\"app\":\"settings\",\"skill\":\"open_pane\",\"args\":{\"pane\":\"sound\"}}");
    struct json_object *invoke = client_wait(&desktop, "skill.invoke", NULL, NULL, 0);
    MARY_ASSERT(invoke != NULL);
    if (invoke) {
        MARY_ASSERT_STR(mc_json_string(invoke, "app"), "settings");
        MARY_ASSERT_STR(mc_json_string(mc_json_object(invoke, "args"), "pane"), "sound");
        char line[256];
        snprintf(line, sizeof line, "{\"type\":\"skill.result\",\"call_id\":\"%s\",\"ok\":true,\"result\":{\"opened\":\"sound\"}}",
                 mc_json_string(invoke, "call_id"));
        client_send(&desktop, line);
        json_object_put(invoke);
    }
    struct json_object *result = client_wait(&ctl, "skill.result", NULL, NULL, 0);
    bool ok = false;
    MARY_ASSERT(result && mc_json_bool(result, "ok", &ok) && ok);
    MARY_ASSERT_STR(mc_json_string(mc_json_object(result, "result"), "opened"), "sound");
    if (result) json_object_put(result);

    const struct {
        const char *call, *error;
    } refused[] = {
        { "{\"type\":\"skill.call\",\"app\":\"media\",\"skill\":\"play_pause\"}", "denied" },
        { "{\"type\":\"skill.call\",\"app\":\"calendar\",\"skill\":\"events_today\"}", "needs_confirmation" },
        { "{\"type\":\"skill.call\",\"app\":\"mail\",\"skill\":\"send\"}", "unknown" },
    };
    for (size_t i = 0; i < sizeof refused / sizeof *refused; i++) {
        client_send(&ctl, refused[i].call);
        result = client_wait(&ctl, "skill.result", NULL, NULL, 0);
        MARY_ASSERT(result != NULL);
        if (result) MARY_ASSERT_STR(mc_json_string(result, "error"), refused[i].error);
        if (result) json_object_put(result);
    }

    client_send(&ctl, "{\"type\":\"skill.call\",\"app\":\"settings\",\"skill\":\"open_pane\"}");
    invoke = client_wait(&desktop, "skill.invoke", NULL, NULL, 0);
    MARY_ASSERT(invoke != NULL);
    if (invoke) json_object_put(invoke);
    client_close(&desktop);                         /* the desktop goes away mid-call */
    result = client_wait(&ctl, "skill.result", NULL, NULL, 0);
    MARY_ASSERT(result != NULL);
    if (result) MARY_ASSERT_STR(mc_json_string(result, "error"), "disconnected");
    if (result) json_object_put(result);
    client_close(&ctl);
    teardown();
}

MARY_TEST(a_spoken_question_is_heard_answered_and_followed_up) {
    setup();
    client c;
    client_open(&c);
    json_object_put(client_wait(&c, "hello", NULL, NULL, 0));
    client_send(&c, "{\"type\":\"listen\"}");
    struct json_object *listening = client_wait(&c, "state", "listening", NULL, 0);
    MARY_ASSERT(listening != NULL);
    if (listening) json_object_put(listening);

    int16_t frame[320];
    for (int f = 0; f < 30; f++) {                 /* 600 ms of voice */
        for (int i = 0; i < 320; i++) frame[i] = (int16_t)(8000 * sin(2 * M_PI * 220 * (f * 320 + i) / 16000.0));
        mr_daemon_hear(maryd, frame, 320);
    }
    memset(frame, 0, sizeof frame);
    for (int f = 0; f < 75; f++) mr_daemon_hear(maryd, frame, 320);    /* 1.5 s of quiet */

    char seen[2048] = "";
    struct json_object *said = NULL;
    for (;;) {
        said = client_wait(&c, "transcript", NULL, seen, sizeof seen);
        bool final = false;
        if (!said || (mc_json_bool(said, "final", &final) && final)) break;
        json_object_put(said);
    }
    MARY_ASSERT(said != NULL);
    if (said) {
        MARY_ASSERT_STR(mc_json_string(said, "text"), "what time is it");
        MARY_ASSERT_STR(mc_json_string(said, "source"), "voice");
        json_object_put(said);
    }
    MARY_ASSERT(strstr(seen, "state:hearing") && strstr(seen, "state:transcribing"));
    MARY_ASSERT(atomic_load(&pcm_bytes) >= 30 * 640);

    struct json_object *end = client_wait(&c, "reply.end", NULL, NULL, 0);
    MARY_ASSERT(end != NULL);
    if (end) json_object_put(end);
    listening = client_wait(&c, "state", "listening", NULL, 0);     /* the follow-up window */
    MARY_ASSERT(listening != NULL);
    if (listening) json_object_put(listening);
    struct json_object *idle = client_wait(&c, "state", "idle", NULL, 0);
    MARY_ASSERT(idle != NULL);
    if (idle) json_object_put(idle);
    MARY_ASSERT_EQ(atomic_load(&transcribes), 2);

    MARY_ASSERT(eventually(&index_calls, 1));
    pthread_mutex_lock(&lock);
    MARY_ASSERT_STR(indexed_question, "what time is it");
    MARY_ASSERT_STR(indexed_source, "voice");
    pthread_mutex_unlock(&lock);
    client_close(&c);
    teardown();
}

int main(void) {
    mc_ignore_sigpipe();
    MARY_RUN(a_typed_question_streams_a_reply_and_is_deposited_in_thread);
    MARY_RUN(skill_calls_go_through_the_desktops_pipes_and_its_policy);
    MARY_RUN(a_spoken_question_is_heard_answered_and_followed_up);
    MARY_TEST_MAIN_END();
}
