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

#include "common/base64.h"
#include "common/frame.h"
#include "common/io.h"
#include "common/json.h"
#include "common/lines.h"
#include "conduit/grpc.h"
#include "conduit/thread.pb-c.h"
#include "mary_test.h"
#include "runtime/daemon.h"
#include "sewn/client.h"

static char dir[64], sewn_sock[128], thread_sock[128], local_sock[128], mary_sock[128];
static int sewn_listener = -1, thread_listener = -1, local_listener = -1;
static pthread_t sewn_thread, thread_thread, local_thread, daemon_thread;
static mr_daemon *maryd;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int transcribes, pcm_bytes, index_calls;
static atomic_int sewn_mode;                /* 1: the reply's voice fails */
static atomic_int voices_calls;
static char last_voice[64], spoken_voice[64];
static int last_turn_messages;
static bool last_turn_instructed, last_turn_saw_screen;
static char last_turn_lanes[128];
static char last_user[128], indexed_question[128], indexed_reply[128], indexed_source[16];
static atomic_int embed_calls, complete_calls;
static char deposited[1024];                /* the families deposited on the local socket, in order */
static char complete_tools[512];            /* the tool names the last complete offered */

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
            last_turn_saw_screen = last_turn_instructed && strstr(mc_json_string(request, "instructions"), "On screen: TextEdit") != NULL;
            struct json_object *lanes = mc_json_array(mc_json_object(request, "sewn"), "lanes");
            snprintf(last_turn_lanes, sizeof last_turn_lanes, "%s", lanes ? mc_json_compact(lanes, NULL) : "");
            const char *voice = mc_json_string(mc_json_object(first, "tts"), "voice_id");
            snprintf(last_voice, sizeof last_voice, "%s", voice ? voice : "");
            if (last_turn_messages)
                snprintf(last_user, sizeof last_user, "%s", mc_json_string(json_object_array_get_idx(messages, last_turn_messages - 1), "content"));
            pthread_mutex_unlock(&lock);
            send_json(fd, "{\"type\":\"phase\",\"phase\":\"grounded\"}");
            send_json(fd, "{\"type\":\"token\",\"phase\":\"grounded\",\"text\":\"Hello\"}");
            send_json(fd, "{\"type\":\"token\",\"phase\":\"grounded\",\"text\":\" there.\"}");
            if (atomic_load(&sewn_mode) == 1) {
                send_json(fd, "{\"type\":\"tts.failed\",\"status\":403,\"message\":\"Mistral refused to speak it (HTTP 403)\"}");
            } else {
                send_json(fd, "{\"type\":\"audio.begin\",\"sample_rate\":24000,\"channels\":1,\"bits\":32,\"encoding\":\"f32le\"}");
                unsigned char pcm[480 * 4] = { 0 };
                mc_frame_write_fd(fd, MC_FRAME_PCM, pcm, sizeof pcm);
            }
            send_json(fd, "{\"type\":\"turn.end\",\"text\":\"Hello there.\",\"contribution\":{\"owners\":[{\"thread_id\":\"\",\"owner_id\":\"mary\","
                          "\"document_ids\":[\"file-1\"],\"influence\":{\"file-1\":1},\"royalty\":1,\"spans\":[{\"lower\":0,\"upper\":12}]}]},"
                          "\"retrieved\":[{\"document_id\":\"file-1\",\"group_id\":\"files-mary\",\"family\":\"file\",\"lane\":\"personal\",\"score\":4.2}]}");
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
        } else if (strcmp(type, "voices.list") == 0) {
            atomic_fetch_add(&voices_calls, 1);
            send_json(fd, "{\"type\":\"voices\",\"voices\":[{\"voice_id\":\"fr_marie_neutral\",\"name\":\"Marie\",\"languages\":[\"fr\"],\"custom\":false},"
                          "{\"voice_id\":\"en_paul_neutral\",\"name\":\"Paul\",\"languages\":[\"en\"],\"custom\":false}]}");
        } else if (strcmp(type, "speak") == 0) {
            const char *voice = mc_json_string(first, "voice_id");
            pthread_mutex_lock(&lock);
            snprintf(spoken_voice, sizeof spoken_voice, "%s", voice ? voice : "");
            pthread_mutex_unlock(&lock);
            send_json(fd, "{\"type\":\"audio.begin\",\"sample_rate\":24000,\"channels\":1,\"bits\":32,\"encoding\":\"f32le\"}");
            unsigned char pcm[240 * 4] = { 0 };
            mc_frame_write_fd(fd, MC_FRAME_PCM, pcm, sizeof pcm);
            send_json(fd, "{\"type\":\"speak.end\"}");
        } else if (strcmp(type, "embed") == 0) {
            /* a bag of words in 64 dimensions: enough for one skill to win on shared words */
            atomic_fetch_add(&embed_calls, 1);
            struct json_object *texts = mc_json_array(first, "texts"), *reply = json_object_new_object(), *vectors = json_object_new_array();
            for (size_t i = 0; texts && i < json_object_array_length(texts); i++) {
                float v[64] = { 0 };
                const char *t = json_object_get_string(json_object_array_get_idx(texts, i));
                unsigned h = 5381;
                bool in_word = false;
                for (const char *c = t;; c++) {
                    bool alnum = *c && ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '\'');
                    if (alnum) { h = h * 33 + (unsigned)(*c | 0x20); in_word = true; }
                    else if (in_word) { v[h % 64] = 1; h = 5381; in_word = false; }
                    if (!*c) break;
                }
                unsigned char bytes[64 * 4];
                for (int k = 0; k < 64; k++) memcpy(bytes + k * 4, &v[k], 4);
                char b64[400];
                size_t written = 0;
                mc_base64_encode(bytes, sizeof bytes, b64, sizeof b64, &written);
                json_object_array_add(vectors, json_object_new_string_len(b64, (int)written));
            }
            json_object_object_add(reply, "type", json_object_new_string("embed.result"));
            json_object_object_add(reply, "model", json_object_new_string("mistral-embed"));
            json_object_object_add(reply, "dim", json_object_new_int(64));
            json_object_object_add(reply, "vectors_b64", vectors);
            send_json(fd, mc_json_compact(reply, NULL));
            json_object_put(reply);
        } else if (strcmp(type, "complete") == 0) {
            /* the skills lane's model: a round with no tool result yet calls the first tool offered; then it answers */
            atomic_fetch_add(&complete_calls, 1);
            struct json_object *messages = mc_json_array(first, "messages"), *tools = mc_json_array(first, "tools");
            bool acted = false;
            for (size_t i = 0; messages && i < json_object_array_length(messages); i++) {
                const char *role = mc_json_string(json_object_array_get_idx(messages, i), "role");
                acted = acted || (role && strcmp(role, "tool") == 0);
            }
            pthread_mutex_lock(&lock);
            complete_tools[0] = 0;
            for (size_t i = 0; tools && i < json_object_array_length(tools); i++) {
                const char *name = mc_json_string(mc_json_object(json_object_array_get_idx(tools, i), "function"), "name");
                size_t used = strlen(complete_tools);
                snprintf(complete_tools + used, sizeof complete_tools - used, "%s ", name ? name : "?");
            }
            const char *tool = tools && json_object_array_length(tools) ? mc_json_string(mc_json_object(json_object_array_get_idx(tools, 0), "function"), "name") : NULL;
            const char *asked = messages && json_object_array_length(messages) ? mc_json_string(json_object_array_get_idx(messages, json_object_array_length(messages) - 1), "content") : NULL;
            for (size_t i = 0; tools && i < json_object_array_length(tools); i++) {
                const char *name = mc_json_string(mc_json_object(json_object_array_get_idx(tools, i), "function"), "name");
                if (name && asked && strstr(name, "calendar") && strstr(asked, "calendar")) tool = name;
            }
            char line[400];
            if (!acted && tool) snprintf(line, sizeof line, "{\"type\":\"complete.result\",\"text\":\"\",\"tool_calls\":[{\"id\":\"c1\",\"name\":\"%s\",\"arguments\":\"{}\"}],\"provider\":\"mistral\"}", tool);
            else snprintf(line, sizeof line, "{\"type\":\"complete.result\",\"text\":\"You have two events today.\",\"tool_calls\":[],\"provider\":\"mistral\"}");
            pthread_mutex_unlock(&lock);
            send_json(fd, line);
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

/* ---- fake threadd's local socket: one deposit per line, its family remembered ---- */

static int on_local_line(const char *line, size_t len, void *user) {
    int fd = (int)(intptr_t)user;
    struct json_object *req = mc_json_parse(line, len);
    const char *family = req ? mc_json_string(req, "family") : NULL;
    pthread_mutex_lock(&lock);
    size_t used = strlen(deposited);
    snprintf(deposited + used, sizeof deposited - used, "%s ", family ? family : "?");
    pthread_mutex_unlock(&lock);
    if (req) json_object_put(req);
    const char *reply = "{\"type\":\"deposit.result\",\"document_id\":\"x\"}\n";
    mc_write_all(fd, reply, strlen(reply));
    return 0;
}

static void *serve_local(void *arg) {
    for (;;) {
        int fd = accept(local_listener, NULL, NULL);
        if (fd < 0) return NULL;
        mc_line_reader lines;
        mc_line_reader_init(&lines, 1 << 20);
        char bytes[8192];
        ssize_t n;
        while ((n = read(fd, bytes, sizeof bytes)) > 0) mc_line_reader_feed(&lines, bytes, (size_t)n, on_local_line, (void *)(intptr_t)fd);
        mc_line_reader_free(&lines);
        close(fd);
    }
}

/* ---- a speaker that takes nothing ---- */

static atomic_int fake_opens, fake_stops, fake_queued;
static atomic_bool fake_broken;
static int fake_speaker;                    /* its address is the handle */

static void *fake_open(mr_capture_fn on_frame, void *user, int *error, void *ops_user) {
    atomic_fetch_add(&fake_opens, 1);
    atomic_store(&fake_broken, false);
    return &fake_speaker;
}
static void fake_close(void *audio) {}
static int fake_capture(void *audio, bool on) { return 0; }
static size_t fake_play(void *audio, const float *samples, size_t count) {
    atomic_fetch_add(&fake_queued, (int)count);
    return count;
}
static void fake_stop(void *audio) {
    atomic_fetch_add(&fake_stops, 1);
    atomic_store(&fake_queued, 0);
}
static size_t fake_queued_samples(void *audio) { return (size_t)atomic_load(&fake_queued); }
static int64_t fake_last_played_ms(void *audio) { return 0; }
static bool fake_is_broken(void *audio) { return atomic_load(&fake_broken); }
static const mr_audio_ops silent_speaker = { fake_open, fake_close, fake_capture, fake_play, fake_stop,
                                             fake_queued_samples, fake_last_played_ms, fake_is_broken };

/* ---- maryd and its clients ---- */

static void *run_daemon(void *arg) {
    mr_daemon_run(maryd);
    return NULL;
}

static void setup_with(const mr_audio_ops *audio_ops, int stall_ms) {
    snprintf(dir, sizeof dir, "/tmp/maryd-XXXXXX");
    MARY_ASSERT(mkdtemp(dir) != NULL);
    snprintf(sewn_sock, sizeof sewn_sock, "%s/sewn.sock", dir);
    snprintf(thread_sock, sizeof thread_sock, "%s/thread.sock", dir);
    snprintf(local_sock, sizeof local_sock, "%s/local.sock", dir);
    snprintf(mary_sock, sizeof mary_sock, "%s/mary.sock", dir);
    sewn_listener = mc_listen_unix(sewn_sock, 0600);
    thread_listener = mc_listen_unix(thread_sock, 0600);
    local_listener = mc_listen_unix(local_sock, 0600);
    pthread_create(&sewn_thread, NULL, serve_sewn, NULL);
    pthread_create(&thread_thread, NULL, serve_thread, NULL);
    pthread_create(&local_thread, NULL, serve_local, NULL);
    atomic_store(&embed_calls, 0);
    atomic_store(&complete_calls, 0);
    deposited[0] = last_user[0] = indexed_reply[0] = complete_tools[0] = 0;
    atomic_store(&index_calls, 0);
    atomic_store(&transcribes, 0);
    atomic_store(&pcm_bytes, 0);
    atomic_store(&sewn_mode, 0);
    atomic_store(&voices_calls, 0);

    mr_config config = mr_config_default();
    config.desktop_socket = mary_sock;
    config.sewn_socket = sewn_sock;
    config.thread_socket = thread_sock;
    config.thread_local_socket = local_sock;
    config.audio = audio_ops != NULL;          /* no PipeWire in a test: a fake speaker, or none */
    config.audio_ops = audio_ops;
    if (stall_ms) config.speaker_stall_ms = stall_ms;
    config.wake = false;
    config.listen_timeout_ms = 400;
    config.echo_tail_ms = 0;
    config.skill_timeout_ms = 2000;
    int error = 0;
    maryd = mr_daemon_new(&config, &error);
    MARY_ASSERT(maryd != NULL);
    if (maryd) pthread_create(&daemon_thread, NULL, run_daemon, NULL);
}

static void setup(void) { setup_with(NULL, 0); }

static void teardown(void) {
    if (maryd) {
        mr_daemon_stop(maryd);
        pthread_join(daemon_thread, NULL);
        mr_daemon_free(maryd);
        maryd = NULL;
    }
    shutdown(sewn_listener, SHUT_RDWR);
    shutdown(thread_listener, SHUT_RDWR);
    shutdown(local_listener, SHUT_RDWR);
    pthread_join(sewn_thread, NULL);
    pthread_join(thread_thread, NULL);
    pthread_join(local_thread, NULL);
    close(sewn_listener);
    close(thread_listener);
    close(local_listener);
    unlink(sewn_sock);
    unlink(thread_sock);
    unlink(local_sock);
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
    /* Gita's contribution and what was retrieved come with the end of the reply (the highlights) */
    struct json_object *owners = mc_json_array(mc_json_object(end, "contribution"), "owners");
    MARY_ASSERT(owners && json_object_array_length(owners) == 1);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(json_object_array_get_idx(owners, 0), "spans")), 1);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(end, "retrieved")), 1);
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

MARY_TEST(the_world_the_desktop_publishes_shapes_the_turn_and_its_trace) {
    setup();
    client desktop, ctl;
    client_open(&desktop);
    client_open(&ctl);
    json_object_put(client_wait(&desktop, "hello", NULL, NULL, 0));
    json_object_put(client_wait(&ctl, "hello", NULL, NULL, 0));
    /* the desktop publishes its skills (so it is the desktop) and what is in front of the person */
    client_send(&desktop, "{\"type\":\"skills\",\"apps\":[{\"id\":\"textedit\",\"name\":\"TextEdit\",\"enabled\":true,\"ask\":\"never\",\"skills\":[]}]}");
    client_send(&desktop,
        "{\"type\":\"world\",\"focus\":\"applications:textedit\",\"places\":[{\"place\":\"applications:textedit\",\"surface\":"
        "{\"application\":{\"name\":\"TextEdit\",\"id\":\"textedit\"},\"activeWindow\":{\"title\":\"Tides\"},\"windowCount\":1,"
        "\"elements\":[{\"role\":\"textarea\",\"kind\":\"text area\",\"label\":\"body\",\"focused\":true}],"
        "\"document\":{\"name\":\"Tides\",\"path\":\"/home/mary/Tides.txt\",\"text\":\"The tide comes in twice a day.\",\"total\":31,\"lower\":0,\"upper\":31}}}]}");
    /* the Ambient app's World tab */
    struct json_object *state = NULL;
    for (int i = 0; i < 50 && !state; i++) {
        client_send(&ctl, "{\"type\":\"ambient.state\"}");
        struct json_object *reply = client_wait(&ctl, "ambient", NULL, NULL, 0);
        struct json_object *places = reply ? mc_json_array(mc_json_object(reply, "state"), "places") : NULL;
        if (places && json_object_array_length(places) == 1) state = reply;
        else { if (reply) json_object_put(reply); usleep(20000); }
    }
    MARY_ASSERT(state != NULL);
    if (state) {
        struct json_object *card = json_object_array_get_idx(mc_json_array(mc_json_object(state, "state"), "places"), 0);
        bool lead = false;
        MARY_ASSERT(mc_json_bool(card, "isLead", &lead) && lead);
        const char *line = mc_json_string(mc_json_object(card, "surface"), "surfaceLine");
        MARY_ASSERT(line && strncmp(line, "On screen: TextEdit \xE2\x80\x94 \"Tides\" (focused: body)", 40) == 0);
        MARY_ASSERT_EQ(json_object_array_length(mc_json_array(card, "facts")), 1);
        MARY_ASSERT_STR(mc_json_string(mc_json_object(mc_json_object(state, "state"), "lead"), "token"), "applications:textedit");
        json_object_put(state);
    }
    /* a turn asks the desktop for the world first, then runs with what it holds */
    client_send(&ctl, "{\"type\":\"ask\",\"text\":\"what does the document say about the tide?\"}");
    struct json_object *request = client_wait(&desktop, "world.request", NULL, NULL, 0);
    MARY_ASSERT(request != NULL);
    if (request) json_object_put(request);
    struct json_object *end = client_wait(&ctl, "reply.end", NULL, NULL, 0);
    MARY_ASSERT(end != NULL);
    if (end) json_object_put(end);
    pthread_mutex_lock(&lock);
    MARY_ASSERT(last_turn_saw_screen);
    MARY_ASSERT_STR(last_turn_lanes, "[\"personal\",\"conversation\"]");
    pthread_mutex_unlock(&lock);
    /* the Routes tab: the turn's row, its route and what retrieval returned */
    client_send(&ctl, "{\"type\":\"trace.list\"}");
    struct json_object *trace = client_wait(&ctl, "trace", NULL, NULL, 0);
    MARY_ASSERT(trace != NULL);
    if (trace) {
        struct json_object *records = mc_json_array(trace, "records");
        MARY_ASSERT(records && json_object_array_length(records) == 1);
        struct json_object *row = json_object_array_get_idx(records, 0), *route = mc_json_object(row, "route");
        MARY_ASSERT_STR(mc_json_string(row, "utterance"), "what does the document say about the tide?");
        MARY_ASSERT_STR(mc_json_string(route, "intent"), "ask");
        MARY_ASSERT_STR(mc_json_string(route, "decidedBy"), "namedPart");
        MARY_ASSERT_STR(mc_json_string(route, "leadApplicationID"), "textedit");
        MARY_ASSERT_STR(mc_json_string(mc_json_object(route, "verdicts"), "namedPart"), "tide");
        struct json_object *retrieval = mc_json_array(row, "retrieval");
        MARY_ASSERT(retrieval && json_object_array_length(retrieval) == 1);
        struct json_object *context = json_object_array_get_idx(retrieval, 0);
        MARY_ASSERT_STR(mc_json_string(context, "name"), "context");
        MARY_ASSERT_EQ(json_object_array_length(mc_json_array(context, "returned")), 1);
        MARY_ASSERT_STR(mc_json_string(row, "contribution"), "1 owner, 1 document, 1 passage");
        int64_t chars = 0;
        MARY_ASSERT(mc_json_int64(row, "systemPromptChars", &chars) && chars > 1000);
        json_object_put(trace);
    }
    client_send(&ctl, "{\"type\":\"trace.report\"}");
    struct json_object *report = client_wait(&ctl, "trace.report", NULL, NULL, 0);
    MARY_ASSERT(report && strstr(mc_json_string(report, "text"), "--- ask via namedPart ---") != NULL);
    if (report) json_object_put(report);
    /* a selection becomes this turn's referent, and a clear forgets it */
    client_send(&desktop, "{\"type\":\"selection\",\"applicationID\":\"textedit\",\"text\":\"twice a day\",\"document\":\"Tides\",\"lower\":18,\"upper\":29,\"total\":31,\"editable\":true}");
    client_send(&ctl, "{\"type\":\"ambient.state\"}");
    state = NULL;
    for (int i = 0; i < 50 && !state; i++) {
        struct json_object *reply = client_wait(&ctl, "ambient", NULL, NULL, 0);
        if (reply && mc_json_object(mc_json_object(reply, "state"), "selection")) state = reply;
        else { if (reply) json_object_put(reply); usleep(20000); client_send(&ctl, "{\"type\":\"ambient.state\"}"); }
    }
    MARY_ASSERT(state != NULL);
    if (state) {
        MARY_ASSERT_STR(mc_json_string(mc_json_object(mc_json_object(state, "state"), "selection"), "text"), "twice a day");
        json_object_put(state);
    }
    /* maryctl's app.state rides the pipes to the desktop and back */
    client_send(&ctl, "{\"type\":\"app.state\",\"app\":\"textedit\"}");
    struct json_object *ask = client_wait(&desktop, "app.state", NULL, NULL, 0);
    MARY_ASSERT(ask != NULL);
    if (ask) {
        char line[256];
        snprintf(line, sizeof line, "{\"type\":\"app.state.result\",\"call_id\":\"%s\",\"ok\":true,\"surface\":{\"application\":{\"name\":\"TextEdit\"}}}", mc_json_string(ask, "call_id"));
        client_send(&desktop, line);
        json_object_put(ask);
    }
    struct json_object *answer = client_wait(&ctl, "app.state.result", NULL, NULL, 0);
    bool ok = false;
    MARY_ASSERT(answer && mc_json_bool(answer, "ok", &ok) && ok);
    MARY_ASSERT_STR(mc_json_string(mc_json_object(mc_json_object(answer, "surface"), "application"), "name"), "TextEdit");
    if (answer) json_object_put(answer);
    client_close(&desktop);
    client_close(&ctl);
    teardown();
}

/* The desktop publishes its skills, and maryd builds the index triage scores against; the caller waits for it. */
static const char *ABILITY_SKILLS =
    "{\"type\":\"skills\",\"apps\":["
    "{\"id\":\"media\",\"name\":\"Media Player\",\"enabled\":true,\"ask\":\"never\",\"summary\":\"Plays music.\",\"discipline\":\"multimedia\",\"skills\":"
    "[{\"id\":\"play_pause\",\"title\":\"Play or pause\",\"summary\":\"Plays or pauses the music.\",\"params\":null,\"effect\":\"act\",\"enabled\":true,"
    "\"triggers\":{\"tokens\":[\"play\",\"pause\"],\"phrases\":[\"play the music\"]}}]},"
    "{\"id\":\"settings\",\"name\":\"System Settings\",\"enabled\":true,\"ask\":\"never\",\"paradigm\":\"systemControl\",\"skills\":"
    "[{\"id\":\"open_pane\",\"title\":\"Open a pane\",\"summary\":\"Opens a settings pane.\",\"params\":{\"type\":\"object\",\"properties\":{\"pane\":{\"type\":\"string\"}},\"required\":[\"pane\"]},\"effect\":\"act\",\"enabled\":true}]},"
    "{\"id\":\"calendar\",\"name\":\"Calendar\",\"enabled\":true,\"ask\":\"always\",\"skills\":"
    "[{\"id\":\"events_today\",\"title\":\"Today's events\",\"summary\":\"Reads what is on the calendar today.\",\"params\":null,\"effect\":\"read\",\"enabled\":true,"
    "\"triggers\":{\"tokens\":[\"calendar\"],\"phrases\":[\"what is on my calendar\"]}}]}]}";

static struct json_object *publish_skills_and_wait_for_triage(client *desktop, client *ctl, const char *rehearse) {
    client_send(desktop, ABILITY_SKILLS);
    struct json_object *result = NULL;
    char line[300];
    snprintf(line, sizeof line, "{\"type\":\"triage\",\"text\":\"%s\"}", rehearse);
    for (int i = 0; i < 100 && !result; i++) {
        client_send(ctl, line);
        struct json_object *reply = NULL;
        for (;;) {                      /* an error while the index is not built yet, or the result */
            reply = client_wait(ctl, "triage.result", NULL, NULL, 0);
            break;
        }
        bool ok = false;
        if (reply && mc_json_bool(reply, "ok", &ok) && ok) result = reply;
        else { if (reply) json_object_put(reply); usleep(30000); }
    }
    return result;
}

MARY_TEST(a_confident_skill_dispatches_without_a_model_round) {
    setup();
    client desktop, ctl;
    client_open(&desktop);
    client_open(&ctl);
    json_object_put(client_wait(&desktop, "hello", NULL, NULL, 0));
    json_object_put(client_wait(&ctl, "hello", NULL, NULL, 0));
    /* the ability records go into the Thread as soon as the skills arrive */
    struct json_object *rehearsal = publish_skills_and_wait_for_triage(&desktop, &ctl, "play the music");
    MARY_ASSERT(rehearsal != NULL);
    if (rehearsal) {
        struct json_object *winner = mc_json_object(rehearsal, "winner");
        bool dispatchable = false;
        MARY_ASSERT(winner != NULL);
        MARY_ASSERT_STR(mc_json_string(winner, "invocation"), "media__play_pause");
        MARY_ASSERT_STR(mc_json_string(winner, "shape"), "noRequiredArguments");
        MARY_ASSERT(mc_json_bool(winner, "dispatchable", &dispatchable) && dispatchable);
        json_object_put(rehearsal);
    }
    MARY_ASSERT(atomic_load(&embed_calls) >= 2);      /* the index, and the rehearsal */
    for (int i = 0; i < 100 && !strstr(deposited, "ability-schema"); i++) usleep(10000);
    pthread_mutex_lock(&lock);
    MARY_ASSERT(strstr(deposited, "ability ") != NULL && strstr(deposited, "ability-schema") != NULL);
    pthread_mutex_unlock(&lock);
    client_send(&ctl, "{\"type\":\"abilities.list\"}");
    struct json_object *abilities = client_wait(&ctl, "abilities", NULL, NULL, 0);
    MARY_ASSERT(abilities && json_object_array_length(mc_json_array(abilities, "records")) >= 6);   /* 3 skills, 3 manifests, a discipline */
    if (abilities) json_object_put(abilities);

    /* the turn: one embedding, one skill, no model */
    int completes = atomic_load(&complete_calls);
    client_send(&ctl, "{\"type\":\"ask\",\"text\":\"play the music\"}");
    struct json_object *invoke = client_wait(&desktop, "skill.invoke", NULL, NULL, 0);
    MARY_ASSERT(invoke != NULL);
    if (invoke) {
        MARY_ASSERT_STR(mc_json_string(invoke, "app"), "media");
        MARY_ASSERT_STR(mc_json_string(invoke, "skill"), "play_pause");
        char line[256];
        snprintf(line, sizeof line, "{\"type\":\"skill.result\",\"call_id\":\"%s\",\"ok\":true,\"result\":{\"landed\":true,\"summary\":\"Playing.\"}}", mc_json_string(invoke, "call_id"));
        client_send(&desktop, line);
        json_object_put(invoke);
    }
    char seen[1024] = "";
    struct json_object *end = client_wait(&ctl, "reply.end", NULL, seen, sizeof seen);
    MARY_ASSERT(end != NULL);
    MARY_ASSERT(strstr(seen, "reply.delta") != NULL);
    if (end) {
        struct json_object *runs = mc_json_array(end, "runs");
        if (!runs || json_object_array_length(runs) != 1) fprintf(stderr, "  reply.end: %s\n  seen: %s\n", mc_json_compact(end, NULL), seen);
        MARY_ASSERT(runs && json_object_array_length(runs) == 1);
        if (runs && json_object_array_length(runs) == 1) {
            struct json_object *run = json_object_array_get_idx(runs, 0);
            bool ok = false;
            MARY_ASSERT_STR(mc_json_string(run, "invocation"), "media__play_pause");
            MARY_ASSERT(mc_json_bool(run, "ok", &ok) && ok);
            MARY_ASSERT_STR(mc_json_string(run, "summary"), "Playing.");
        }
        json_object_put(end);
    }
    MARY_ASSERT_EQ(atomic_load(&complete_calls), completes);
    pthread_mutex_lock(&lock);
    MARY_ASSERT_STR(last_user, "");                 /* sewnd's turn.start never ran */
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < 100 && !strstr(deposited, "interaction"); i++) usleep(10000);
    pthread_mutex_lock(&lock);
    MARY_ASSERT(strstr(deposited, "routing ") != NULL);
    MARY_ASSERT(strstr(deposited, "behavior ") != NULL && strstr(deposited, "interaction ") != NULL);
    pthread_mutex_unlock(&lock);
    MARY_ASSERT(eventually(&index_calls, 1));      /* the conversation record too */
    client_send(&ctl, "{\"type\":\"trace.list\"}");
    struct json_object *trace = client_wait(&ctl, "trace", NULL, NULL, 0);
    MARY_ASSERT(trace != NULL);
    if (trace) {
        struct json_object *row = json_object_array_get_idx(mc_json_array(trace, "records"), 0), *runs = mc_json_array(row, "skillRuns");
        MARY_ASSERT(runs && json_object_array_length(runs) == 1);
        if (runs && json_object_array_length(runs)) MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(runs, 0), "status"), "completed");
        json_object_put(trace);
    }
    client_close(&desktop);
    client_close(&ctl);
    teardown();
}

MARY_TEST(an_action_turn_runs_the_skills_lane_and_parks_a_protected_skill) {
    setup();
    client desktop, ctl;
    client_open(&desktop);
    client_open(&ctl);
    json_object_put(client_wait(&desktop, "hello", NULL, NULL, 0));
    json_object_put(client_wait(&ctl, "hello", NULL, NULL, 0));
    struct json_object *rehearsal = publish_skills_and_wait_for_triage(&desktop, &ctl, "what is on my calendar");
    MARY_ASSERT(rehearsal != NULL);
    if (rehearsal) {
        struct json_object *winner = mc_json_object(rehearsal, "winner");
        bool dispatchable = true;
        MARY_ASSERT(winner != NULL);
        MARY_ASSERT_STR(mc_json_string(winner, "invocation"), "calendar__events_today");
        MARY_ASSERT_STR(mc_json_string(winner, "decision"), "needs_confirmation");
        MARY_ASSERT(mc_json_bool(winner, "dispatchable", &dispatchable) && !dispatchable);
        json_object_put(rehearsal);
    }
    /* a unique winner that cannot dispatch hands the turn to the skills lane, which parks the call on the card */
    client_send(&ctl, "{\"type\":\"ask\",\"text\":\"what is on my calendar\"}");
    char before[1024] = "";
    struct json_object *card = client_wait(&desktop, "skill.confirm", NULL, before, sizeof before);
    if (!card) fprintf(stderr, "  no card; the desktop saw: %s\n", before);
    MARY_ASSERT(card != NULL);
    if (card) {
        MARY_ASSERT_STR(mc_json_string(card, "app"), "calendar");
        MARY_ASSERT_STR(mc_json_string(card, "skill"), "events_today");
        MARY_ASSERT_STR(mc_json_string(card, "title"), "Today's events");
        MARY_ASSERT_STR(mc_json_string(card, "summary"), "Today's events in Calendar?");
        char line[256];
        snprintf(line, sizeof line, "{\"type\":\"skill.confirm.reply\",\"call_id\":\"%s\",\"yes\":true}", mc_json_string(card, "call_id"));
        client_send(&desktop, line);
        json_object_put(card);
    }
    struct json_object *invoke = client_wait(&desktop, "skill.invoke", NULL, NULL, 0);
    MARY_ASSERT(invoke != NULL);
    if (invoke) {
        MARY_ASSERT_STR(mc_json_string(invoke, "app"), "calendar");
        char line[256];
        snprintf(line, sizeof line, "{\"type\":\"skill.result\",\"call_id\":\"%s\",\"ok\":true,\"result\":{\"events\":2}}", mc_json_string(invoke, "call_id"));
        client_send(&desktop, line);
        json_object_put(invoke);
    }
    char seen[1024] = "";
    struct json_object *end = client_wait(&ctl, "reply.end", NULL, seen, sizeof seen);
    MARY_ASSERT(end != NULL);
    MARY_ASSERT(strstr(seen, "reply.delta") != NULL);
    if (end) {
        struct json_object *runs = mc_json_array(end, "runs");
        MARY_ASSERT(runs && json_object_array_length(runs) == 1);
        if (runs && json_object_array_length(runs) == 1) {
            bool ok = false, requested = true;
            struct json_object *run = json_object_array_get_idx(runs, 0);
            MARY_ASSERT(mc_json_bool(run, "ok", &ok) && ok);
            MARY_ASSERT(mc_json_bool(run, "requested", &requested) && !requested);
        }
        json_object_put(end);
    }
    MARY_ASSERT_EQ(atomic_load(&complete_calls), 2);
    pthread_mutex_lock(&lock);
    MARY_ASSERT(strstr(complete_tools, "calendar__events_today") != NULL && strstr(complete_tools, "media__play_pause") != NULL);
    pthread_mutex_unlock(&lock);
    MARY_ASSERT(eventually(&index_calls, 1));
    pthread_mutex_lock(&lock);
    MARY_ASSERT_STR(indexed_reply, "You have two events today.");
    pthread_mutex_unlock(&lock);
    client_send(&ctl, "{\"type\":\"trace.list\"}");
    struct json_object *trace = client_wait(&ctl, "trace", NULL, NULL, 0);
    MARY_ASSERT(trace != NULL);
    if (trace) {
        struct json_object *row = json_object_array_get_idx(mc_json_array(trace, "records"), 0);
        MARY_ASSERT_STR(mc_json_string(row, "confirmation"), "allowed");
        MARY_ASSERT_EQ(json_object_array_length(mc_json_array(row, "skillRuns")), 1);
        json_object_put(trace);
    }
    /* a second turn while nothing is parked: a bare "yes" is a turn of its own */
    client_send(&ctl, "{\"type\":\"skill.confirm.reply\",\"call_id\":\"nothing\",\"yes\":true}");
    struct json_object *err = client_wait(&ctl, "error", NULL, NULL, 0);
    MARY_ASSERT(err && strcmp(mc_json_string(err, "stage"), "skill.confirm") == 0);
    if (err) json_object_put(err);
    client_close(&desktop);
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

MARY_TEST(speech_that_fails_is_reported_and_mary_goes_quiet) {
    setup();
    atomic_store(&sewn_mode, 1);
    client c;
    client_open(&c);
    json_object_put(client_wait(&c, "hello", NULL, NULL, 0));
    client_send(&c, "{\"type\":\"ask\",\"text\":\"Say hello\"}");
    char seen[1024] = "";
    struct json_object *failed = client_wait(&c, "error", NULL, seen, sizeof seen);
    MARY_ASSERT(failed != NULL);
    if (failed) {
        MARY_ASSERT_STR(mc_json_string(failed, "stage"), "speech");
        MARY_ASSERT_STR(mc_json_string(failed, "message"), "Mistral refused to speak it (HTTP 403)");
        json_object_put(failed);
    }
    struct json_object *end = client_wait(&c, "reply.end", NULL, seen, sizeof seen);
    MARY_ASSERT(end != NULL);
    if (end) json_object_put(end);
    struct json_object *idle = client_wait(&c, "state", "idle", seen, sizeof seen);
    MARY_ASSERT(idle != NULL);
    if (idle) json_object_put(idle);
    if (!strstr(seen, "reply.delta") || strstr(seen, "state:speaking") || strstr(seen, "state:error"))
        MARY_FAIL("a reply whose voice failed went: %s", seen);
    client_close(&c);
    teardown();
}

MARY_TEST(a_speaker_that_never_plays_does_not_keep_mary_speaking) {
    atomic_store(&fake_opens, 0);
    atomic_store(&fake_stops, 0);
    atomic_store(&fake_queued, 0);
    setup_with(&silent_speaker, 300);
    MARY_ASSERT_EQ(atomic_load(&fake_opens), 1);
    client c;
    client_open(&c);
    json_object_put(client_wait(&c, "hello", NULL, NULL, 0));
    client_send(&c, "{\"type\":\"ask\",\"text\":\"Say hello\"}");
    char seen[1024] = "";
    struct json_object *speaking = client_wait(&c, "state", "speaking", seen, sizeof seen);
    MARY_ASSERT(speaking != NULL);
    if (speaking) json_object_put(speaking);
    int64_t since = mc_now_ms();
    struct json_object *err = client_wait(&c, "error", NULL, seen, sizeof seen);
    MARY_ASSERT(err != NULL);
    if (err) {
        MARY_ASSERT_STR(mc_json_string(err, "stage"), "speaker");
        json_object_put(err);
    }
    struct json_object *idle = client_wait(&c, "state", "idle", seen, sizeof seen);
    MARY_ASSERT(idle != NULL);
    if (idle) json_object_put(idle);
    MARY_ASSERT(mc_now_ms() - since < 3000);
    MARY_ASSERT_EQ(atomic_load(&fake_stops), 1);  /* the stalled voice was dropped once */
    MARY_ASSERT_EQ(atomic_load(&fake_queued), 0);
    client_close(&c);
    teardown();
}

MARY_TEST(a_speaker_that_breaks_is_opened_again_while_mary_is_idle) {
    atomic_store(&fake_opens, 0);
    setup_with(&silent_speaker, 300);
    MARY_ASSERT_EQ(atomic_load(&fake_opens), 1);
    atomic_store(&fake_broken, true);          /* PipeWire restarted */
    for (int i = 0; i < 300 && atomic_load(&fake_opens) < 2; i++) usleep(10000);
    MARY_ASSERT_EQ(atomic_load(&fake_opens), 2);
    teardown();
}

MARY_TEST(the_voice_settings_chose_is_the_one_sewnd_is_asked_for) {
    setup();
    client c;
    client_open(&c);
    struct json_object *hello = client_wait(&c, "hello", NULL, NULL, 0);
    MARY_ASSERT(hello && mc_json_string(hello, "voice") && strcmp(mc_json_string(hello, "voice"), "fr_marie_neutral") == 0);
    if (hello) json_object_put(hello);
    client_send(&c, "{\"type\":\"config\",\"voice\":\"en_paul_neutral\"}");
    client_send(&c, "{\"type\":\"ask\",\"text\":\"Say hello\"}");
    struct json_object *end = client_wait(&c, "reply.end", NULL, NULL, 0);
    MARY_ASSERT(end != NULL);
    if (end) json_object_put(end);
    pthread_mutex_lock(&lock);
    MARY_ASSERT_STR(last_voice, "en_paul_neutral");
    pthread_mutex_unlock(&lock);
    client_send(&c, "{\"type\":\"config\",\"voice\":\"not a voice!\"}");
    struct json_object *refused = client_wait(&c, "error", NULL, NULL, 0);
    MARY_ASSERT(refused && strcmp(mc_json_string(refused, "stage"), "config") == 0);
    if (refused) json_object_put(refused);
    client late;
    client_open(&late);
    hello = client_wait(&late, "hello", NULL, NULL, 0);
    MARY_ASSERT(hello && strcmp(mc_json_string(hello, "voice"), "en_paul_neutral") == 0);   /* the refused one changed nothing */
    if (hello) json_object_put(hello);
    client_close(&late);
    client_close(&c);
    teardown();
}

MARY_TEST(voices_go_to_the_client_that_asked_then_come_from_the_cache) {
    setup();
    client c, other;
    client_open(&c);
    client_open(&other);
    json_object_put(client_wait(&c, "hello", NULL, NULL, 0));
    json_object_put(client_wait(&other, "hello", NULL, NULL, 0));
    client_send(&c, "{\"type\":\"voices.list\"}");
    struct json_object *voices = client_wait(&c, "voices", NULL, NULL, 0);
    bool ok = false;
    MARY_ASSERT(voices && mc_json_bool(voices, "ok", &ok) && ok);
    MARY_ASSERT(voices && json_object_array_length(mc_json_array(voices, "voices")) == 2);
    if (voices) json_object_put(voices);
    char seen[256] = "";
    client_send(&other, "{\"type\":\"skills.list\"}");
    struct json_object *skills = client_wait(&other, "skills", NULL, seen, sizeof seen);
    MARY_ASSERT(skills != NULL && strstr(seen, "voices") == NULL);    /* the list went only to the client that asked */
    if (skills) json_object_put(skills);
    client_send(&other, "{\"type\":\"voices.list\"}");
    voices = client_wait(&other, "voices", NULL, NULL, 0);
    MARY_ASSERT(voices != NULL);
    if (voices) json_object_put(voices);
    MARY_ASSERT_EQ(atomic_load(&voices_calls), 1);                   /* the second came from the cache */
    client_close(&other);
    client_close(&c);
    teardown();
}

MARY_TEST(a_sample_plays_without_joining_the_conversation) {
    setup();
    client c;
    client_open(&c);
    json_object_put(client_wait(&c, "hello", NULL, NULL, 0));
    client_send(&c, "{\"type\":\"voice.sample\",\"voice_id\":\"fr_marie_happy\",\"text\":\"Bonjour !\"}");
    char states[128] = "";
    for (int i = 0; i < 4; i++) {
        struct json_object *s = client_wait(&c, "voice.sample", NULL, NULL, 0);
        if (!s) break;
        const char *state = mc_json_string(s, "state");
        size_t used = strlen(states);
        snprintf(states + used, sizeof states - used, "%s ", state ? state : "?");
        bool last = !state || strcmp(state, "done") == 0 || strcmp(state, "failed") == 0;
        json_object_put(s);
        if (last) break;
    }
    MARY_ASSERT_STR(states, "asking playing done ");
    pthread_mutex_lock(&lock);
    MARY_ASSERT_STR(spoken_voice, "fr_marie_happy");
    pthread_mutex_unlock(&lock);
    client_send(&c, "{\"type\":\"voice.sample\",\"voice_id\":\"../bad\"}");
    struct json_object *refused = client_wait(&c, "voice.sample", NULL, NULL, 0);
    MARY_ASSERT(refused && strcmp(mc_json_string(refused, "state"), "failed") == 0);
    if (refused) json_object_put(refused);
    client late;
    client_open(&late);
    struct json_object *hello = client_wait(&late, "hello", NULL, NULL, 0);
    MARY_ASSERT(hello && json_object_array_length(mc_json_array(hello, "tail")) == 0);   /* nothing joined the conversation */
    if (hello) json_object_put(hello);
    usleep(200000);
    MARY_ASSERT_EQ(atomic_load(&index_calls), 0);                    /* nor Thread */
    client_close(&late);
    client_close(&c);
    teardown();
}

int main(void) {
    mc_ignore_sigpipe();
    MARY_RUN(a_typed_question_streams_a_reply_and_is_deposited_in_thread);
    MARY_RUN(skill_calls_go_through_the_desktops_pipes_and_its_policy);
    MARY_RUN(the_world_the_desktop_publishes_shapes_the_turn_and_its_trace);
    MARY_RUN(a_confident_skill_dispatches_without_a_model_round);
    MARY_RUN(an_action_turn_runs_the_skills_lane_and_parks_a_protected_skill);
    MARY_RUN(a_spoken_question_is_heard_answered_and_followed_up);
    MARY_RUN(speech_that_fails_is_reported_and_mary_goes_quiet);
    MARY_RUN(a_speaker_that_never_plays_does_not_keep_mary_speaking);
    MARY_RUN(a_speaker_that_breaks_is_opened_again_while_mary_is_idle);
    MARY_RUN(the_voice_settings_chose_is_the_one_sewnd_is_asked_for);
    MARY_RUN(voices_go_to_the_client_that_asked_then_come_from_the_cache);
    MARY_RUN(a_sample_plays_without_joining_the_conversation);
    MARY_TEST_MAIN_END();
}
