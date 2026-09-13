/* sewnd's voices.list and speak against a scripted Mistral: every page of the list read, slugs preferred and the
 * account's own voices marked, refusals said in words, nothing without a key, a sample streamed and ended, a
 * refused sample ending with tts.failed, and a strange voice or too much text refused before Mistral is asked.
 * Anyone in the sewn group may list and speak; only key.set needs the admin group. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/base64.h"
#include "common/buf.h"
#include "common/frame.h"
#include "common/io.h"
#include "common/json.h"
#include "mary_test.h"
#include "sewn/client.h"
#include "sewn/mistral.h"
#include "sewn/server.h"
#include "sewn/voices.h"

static const char *KEY = "abcdEFGH1234ijklMNOP5678qrst";
static char dir[64];
static sewn_service svc;

/* MARK: - A scripted Mistral */

static const char *pages[3];
static long list_status;
static int list_rc, lists;
static char paths[4][256];

static int scripted_get(const char *path, const char *key, mc_buf *body, size_t max, long *status, char *message, size_t cap, void *user) {
    if (strcmp(key, KEY) != 0) MARY_FAIL("the transport was handed another key");
    if (lists < 4) snprintf(paths[lists], sizeof paths[0], "%s", path);
    int page = lists++;
    if (list_rc < 0) {
        snprintf(message, cap, "the network is down");
        return list_rc;
    }
    *status = list_status;
    const char *text = page < 3 && pages[page] ? pages[page] : "{\"items\":[],\"total\":0}";
    mc_buf_append(body, text, strlen(text));
    return 0;
}

static char speech[1024], spoken[1024];
static long speech_status;

static int scripted_post(const char *path, const char *key, const char *body, size_t body_len, sewn_bytes_fn on_bytes,
                         sewn_stop_fn should_stop, void *user, long *status, char *message, size_t cap, void *transport_user) {
    if (strcmp(key, KEY) != 0) MARY_FAIL("the transport was handed another key");
    snprintf(spoken, sizeof spoken, "%.*s", (int)body_len, body);
    *status = speech_status;
    size_t n = strlen(speech);
    for (size_t i = 0; i < n; i += 5) {
        if (should_stop(user)) return -ECANCELED;
        if (on_bytes(speech + i, n - i < 5 ? n - i : 5, *status, user)) return SEWN_STREAM_STOPPED;
    }
    return 0;
}

static bool uid_1000_administers(uid_t uid, const char *group) { return uid == 1000; }

static void setup(bool with_key) {
    snprintf(dir, sizeof dir, "/tmp/sewn-voices-XXXXXX");
    MARY_ASSERT(mkdtemp(dir) != NULL);
    sewn_service_init(&svc, dir);
    svc.in_group = uid_1000_administers;
    svc.get = scripted_get;
    svc.post_stream = scripted_post;
    memset(pages, 0, sizeof pages);
    list_status = 200;
    list_rc = lists = 0;
    speech_status = 200;
    spoken[0] = 0;
    const unsigned char samples[8] = { 0, 0, 128, 63, 0, 0, 128, 191 };   /* 1.0, -1.0 */
    char b64[16];
    mc_base64_encode(samples, sizeof samples, b64, sizeof b64, NULL);
    snprintf(speech, sizeof speech,
             "event: speech.audio.delta\ndata: {\"event\":\"speech.audio.delta\",\"data\":{\"audio_data\":\"%s\"}}\n\n"
             "event: speech.audio.done\ndata: {\"event\":\"speech.audio.done\",\"data\":{}}\n\n", b64);
    if (with_key) MARY_ASSERT_EQ(sewn_key_store_set(&svc.keys, KEY, strlen(KEY)), 0);
}

static void teardown(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/mistral.key", dir);
    unlink(path);
    rmdir(dir);
}

/* MARK: - One connection, and everything it answers */

struct heard {
    char types[16][24];
    int count;
    size_t pcm;
    char stage[32], message[256];
    long status;
    struct json_object *voices;
};

static int on_frame(uint8_t kind, const unsigned char *bytes, size_t len, void *user) {
    struct heard *h = user;
    if (kind == MC_FRAME_PCM) {
        h->pcm += len;
        if (h->count < 16) snprintf(h->types[h->count++], 24, "pcm");
        return 0;
    }
    struct json_object *msg = mc_json_parse((const char *)bytes, len);
    const char *type = mc_json_type(msg);
    if (h->count < 16) snprintf(h->types[h->count++], 24, "%s", type ? type : "?");
    if (type && (strcmp(type, "error") == 0 || strcmp(type, "tts.failed") == 0)) {
        const char *stage = mc_json_string(msg, "stage"), *message = mc_json_string(msg, "message");
        snprintf(h->stage, sizeof h->stage, "%s", stage ? stage : "");
        snprintf(h->message, sizeof h->message, "%s", message ? message : "");
        int64_t status = 0;
        mc_json_int64(msg, "status", &status);
        h->status = (long)status;
    }
    if (type && strcmp(type, "voices") == 0) h->voices = json_object_get(mc_json_array(msg, "voices"));
    json_object_put(msg);
    return 0;
}

struct serving {
    int fd;
    uid_t uid;
};

static void *serve(void *arg) {
    struct serving *s = arg;
    sewn_peer peer = { .uid = s->uid, .gid = s->uid, .pid = 1, .known = true };
    sewn_serve_connection(&svc, s->fd, &peer);
    close(s->fd);
    return NULL;
}

static void ask(uid_t uid, const char *request, struct heard *h) {
    memset(h, 0, sizeof *h);
    int sv[2];
    MARY_ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    MARY_ASSERT_EQ(mc_frame_write_fd(sv[0], MC_FRAME_JSON, (const unsigned char *)request, strlen(request)), 0);
    struct serving serving = { sv[1], uid };
    pthread_t thread;
    pthread_create(&thread, NULL, serve, &serving);
    mc_frame_reader reader;
    mc_frame_reader_init(&reader, 0, false);
    while (mc_frame_reader_read_fd(&reader, sv[0], on_frame, h) == MC_IO_OK) {}
    mc_frame_reader_free(&reader);
    pthread_join(thread, NULL);
    close(sv[0]);
}

/* MARK: - The list */

MARY_TEST(voices_are_read_from_every_page) {
    setup(true);
    pages[0] = "{\"items\":["
               "{\"id\":\"0fda0527-d5e8-4996-bb0a-afd589ad3ce2\",\"slug\":\"fr_marie_neutral\",\"name\":\"Marie\",\"languages\":[\"fr\"],\"gender\":\"female\",\"user_id\":null},"
               "{\"id\":\"019b2bd7-96e7-7219-8c0b-45a73da50088\",\"slug\":null,\"name\":\"My voice\",\"languages\":[\"en\",\"fr\"],\"user_id\":\"u-1\"},"
               "{\"id\":\"not sendable!\",\"slug\":\"\",\"name\":\"Odd\",\"languages\":[]}],\"page\":1,\"total\":101}";
    pages[1] = "{\"items\":[{\"id\":\"b1\",\"slug\":\"en_paul_neutral\",\"name\":\"Paul\",\"languages\":[\"en\"]}],\"page\":2,\"total\":101}";
    struct heard h;
    ask(1001, "{\"type\":\"voices.list\"}", &h);                   /* not an administrator: listing is anyone's */
    MARY_ASSERT_STR(h.types[0], "voices");
    MARY_ASSERT_EQ(lists, 2);
    MARY_ASSERT_STR(paths[0], "/v1/audio/voices?type=all&limit=100&offset=0");
    MARY_ASSERT_STR(paths[1], "/v1/audio/voices?type=all&limit=100&offset=100");
    MARY_ASSERT(h.voices && json_object_array_length(h.voices) == 3);
    if (h.voices && json_object_array_length(h.voices) == 3) {
        struct json_object *marie = json_object_array_get_idx(h.voices, 0), *mine = json_object_array_get_idx(h.voices, 1),
                           *paul = json_object_array_get_idx(h.voices, 2);
        bool custom = true;
        MARY_ASSERT_STR(mc_json_string(marie, "voice_id"), "fr_marie_neutral");
        MARY_ASSERT_STR(mc_json_string(marie, "name"), "Marie");
        MARY_ASSERT_STR(mc_json_string(marie, "gender"), "female");
        MARY_ASSERT(mc_json_bool(marie, "custom", &custom) && !custom);
        MARY_ASSERT_STR(mc_json_string(mine, "voice_id"), "019b2bd7-96e7-7219-8c0b-45a73da50088");   /* no slug: the id */
        MARY_ASSERT(mc_json_bool(mine, "custom", &custom) && custom);
        MARY_ASSERT_EQ(json_object_array_length(mc_json_array(mine, "languages")), 2);
        MARY_ASSERT_STR(mc_json_string(paul, "voice_id"), "en_paul_neutral");
    }
    json_object_put(h.voices);
    teardown();
}

MARY_TEST(a_refused_list_says_why) {
    setup(true);
    struct heard h;
    list_status = 401;
    pages[0] = "{\"detail\":\"Unauthorized\"}";
    ask(1001, "{\"type\":\"voices.list\"}", &h);
    MARY_ASSERT_STR(h.stage, "key");
    MARY_ASSERT_STR(h.message, "Mistral rejected the key");
    lists = 0;
    list_status = 500;
    pages[0] = "{\"message\":\"upstream timed out\"}";
    ask(1001, "{\"type\":\"voices.list\"}", &h);
    MARY_ASSERT_STR(h.stage, "voices");
    MARY_ASSERT_STR(h.message, "Mistral answered HTTP 500: upstream timed out");
    lists = 0;
    list_status = 200;
    pages[0] = "[1,2,3]";
    ask(1001, "{\"type\":\"voices.list\"}", &h);
    MARY_ASSERT_STR(h.stage, "voices");
    MARY_ASSERT_STR(h.message, "Mistral's list of voices could not be read");
    lists = 0;
    list_rc = -EIO;
    ask(1001, "{\"type\":\"voices.list\"}", &h);
    MARY_ASSERT_STR(h.stage, "network");
    MARY_ASSERT_STR(h.message, "the network is down");
    teardown();
}

MARY_TEST(no_key_no_list_and_no_speech) {
    setup(false);
    struct heard h;
    ask(1001, "{\"type\":\"voices.list\"}", &h);
    MARY_ASSERT_STR(h.stage, "key");
    MARY_ASSERT_EQ(lists, 0);
    ask(1001, "{\"type\":\"speak\",\"voice_id\":\"fr_marie_neutral\",\"text\":\"Bonjour\"}", &h);
    MARY_ASSERT_STR(h.stage, "key");
    MARY_ASSERT_EQ(h.count, 1);
    MARY_ASSERT_STR(spoken, "");
    teardown();
}

/* MARK: - A sample */

MARY_TEST(speak_streams_then_ends) {
    setup(true);
    struct heard h;
    ask(1001, "{\"type\":\"speak\",\"voice_id\":\"fr_marie_happy\",\"text\":\"Bonjour ! Voici la voix que j\xE2\x80\x99" "aurai.\"}", &h);
    MARY_ASSERT_STR(h.types[0], "audio.begin");
    MARY_ASSERT_EQ(h.pcm, 8);
    MARY_ASSERT_STR(h.types[h.count - 1], "speak.end");
    MARY_ASSERT_EQ(h.stage[0], 0);
    struct json_object *body = mc_json_parse(spoken, strlen(spoken));
    MARY_ASSERT_STR(mc_json_string(body, "voice_id"), "fr_marie_happy");
    MARY_ASSERT_STR(mc_json_string(body, "model"), SEWN_TTS_MODEL);
    MARY_ASSERT(mc_json_string(body, "input") && strstr(mc_json_string(body, "input"), "Bonjour") != NULL);
    json_object_put(body);
    teardown();
}

MARY_TEST(speak_that_fails_ends_with_tts_failed) {
    setup(true);
    speech_status = 403;
    snprintf(speech, sizeof speech, "{\"message\":\"Blocked by moderation\"}");
    struct heard h;
    ask(1001, "{\"type\":\"speak\",\"voice_id\":\"fr_marie_neutral\",\"text\":\"Hello there.\"}", &h);
    MARY_ASSERT_EQ(h.count, 2);
    MARY_ASSERT_STR(h.types[0], "tts.failed");
    MARY_ASSERT_EQ(h.status, 403);
    MARY_ASSERT_STR(h.message, "Mistral refused to speak it (HTTP 403: Blocked by moderation)");
    MARY_ASSERT_STR(h.types[1], "speak.end");
    teardown();
}

MARY_TEST(speak_refuses_a_strange_voice_or_too_much_text) {
    setup(true);
    struct heard h;
    ask(1001, "{\"type\":\"speak\",\"voice_id\":\"../../etc\",\"text\":\"hi\"}", &h);
    MARY_ASSERT_STR(h.stage, "request");
    ask(1001, "{\"type\":\"speak\",\"voice_id\":\"fr_marie_neutral\"}", &h);
    MARY_ASSERT_STR(h.stage, "request");
    char text[SEWN_SPEAK_TEXT_MAX + 2], request[SEWN_SPEAK_TEXT_MAX + 128];
    memset(text, 'a', sizeof text - 1);
    text[sizeof text - 1] = 0;
    snprintf(request, sizeof request, "{\"type\":\"speak\",\"voice_id\":\"fr_marie_neutral\",\"text\":\"%s\"}", text);
    ask(1001, request, &h);
    MARY_ASSERT_STR(h.stage, "request");
    MARY_ASSERT_STR(spoken, "");                                     /* Mistral was never asked */
    MARY_ASSERT(sewn_voice_id_valid("0fda0527-d5e8-4996-bb0a-afd589ad3ce2"));
    MARY_ASSERT(!sewn_voice_id_valid("") && !sewn_voice_id_valid("a b") && !sewn_voice_id_valid(NULL));
    teardown();
}

int main(void) {
    mc_ignore_sigpipe();
    MARY_RUN(voices_are_read_from_every_page);
    MARY_RUN(a_refused_list_says_why);
    MARY_RUN(no_key_no_list_and_no_speech);
    MARY_RUN(speak_streams_then_ends);
    MARY_RUN(speak_that_fails_ends_with_tts_failed);
    MARY_RUN(speak_refuses_a_strange_voice_or_too_much_text);
    MARY_TEST_MAIN_END();
}
