#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/base64.h"
#include "common/frame.h"
#include "common/io.h"
#include "common/json.h"
#include "mary_test.h"
#include "sewn/mistral.h"
#include "sewn/server.h"
#include "sewn/transcribe.h"

static const char *KEY = "abcdEFGH1234ijklMNOP5678qrst";

/* MARK: - A scripted Voxtral */

struct voxtral {
    char key[64];
    char path[192];
    char types[64][32];
    int count;
    int appends;
    size_t appended_bytes;
    int64_t sample_rate;
    bool closed;
    bool fail_open;
    bool error_on_end;
    bool never_done;
    sewn_ws_message_fn on_message;
    void *user;
};

static struct voxtral vox;

static void emit(const char *json) { vox.on_message(json, strlen(json), vox.user); }

static sewn_ws *fake_open(const char *path, const char *key, sewn_ws_message_fn on_message, sewn_ws_closed_fn on_closed,
                          void *user, char *message, size_t cap, void *transport_user) {
    snprintf(vox.key, sizeof vox.key, "%s", key);
    snprintf(vox.path, sizeof vox.path, "%s", path);
    if (vox.fail_open) {
        snprintf(message, cap, "could not reach Voxtral: refused");
        return NULL;
    }
    vox.on_message = on_message;
    vox.user = user;
    emit("{\"type\":\"session.created\",\"session\":{}}");
    return (sewn_ws *)&vox;
}

static int fake_send(sewn_ws *ws, const char *text, size_t len) {
    struct json_object *msg = mc_json_parse(text, len);
    const char *type = mc_json_type(msg);
    if (vox.count < 64) snprintf(vox.types[vox.count++], 32, "%s", type ? type : "?");
    if (type && strcmp(type, "session.update") == 0) {
        mc_json_int64(mc_json_object(mc_json_object(msg, "session"), "audio_format"), "sample_rate", &vox.sample_rate);
        emit("{\"type\":\"session.updated\",\"session\":{}}");
    } else if (type && strcmp(type, "input_audio.append") == 0) {
        size_t n = 0;
        const char *b64 = mc_json_string(msg, "audio");
        unsigned char *pcm = mc_base64_decode_alloc(b64, strlen(b64), &n);
        if (n > SEWN_STT_MAX_APPEND) MARY_FAIL("an append of %zu bytes is over Voxtral's limit", n);
        vox.appended_bytes += n;
        vox.appends++;
        free(pcm);
    } else if (type && strcmp(type, "input_audio.end") == 0) {
        if (vox.error_on_end) {
            emit("{\"type\":\"error\",\"error\":{\"message\":\"Invalid audio\",\"code\":4001}}");
        } else if (!vox.never_done) {
            emit("{\"type\":\"transcription.text.delta\",\"text\":\"what is the capital\"}");
            emit("{\"type\":\"transcription.text.delta\",\"text\":\" of France\"}");
            emit("{\"type\":\"transcription.done\",\"text\":\"What is the capital of France?\",\"language\":\"en\"}");
        }
    }
    json_object_put(msg);
    return 0;
}

static void fake_close(sewn_ws *ws) { vox.closed = true; }

static const sewn_ws_ops fake_ops = { fake_open, fake_send, fake_close };

/* MARK: - A client */

static char dir[64];
static sewn_service svc;

static void setup(bool with_key) {
    memset(&vox, 0, sizeof vox);
    snprintf(dir, sizeof dir, "/tmp/sewn-stt-XXXXXX");
    MARY_ASSERT(mkdtemp(dir) != NULL);
    sewn_service_init(&svc, dir);
    svc.ws = &fake_ops;
    svc.transcribe_wait_ms = 200;
    if (with_key) MARY_ASSERT_EQ(sewn_key_store_set(&svc.keys, KEY, strlen(KEY)), 0);
}

static void teardown(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/mistral.key", dir);
    unlink(path);
    rmdir(dir);
}

struct heard {
    char types[32][32];
    int count;
    char deltas[256];
    char done[256];
    char error_stage[32];
    char error_message[256];
};

static int on_frame(uint8_t kind, const unsigned char *bytes, size_t len, void *user) {
    struct heard *h = user;
    struct json_object *msg = kind == MC_FRAME_JSON ? mc_json_parse((const char *)bytes, len) : NULL;
    const char *type = mc_json_type(msg);
    if (h->count < 32) snprintf(h->types[h->count++], 32, "%s", type ? type : "?");
    if (type && strcmp(type, "transcript.delta") == 0) strncat(h->deltas, mc_json_string(msg, "text"), sizeof h->deltas - strlen(h->deltas) - 1);
    if (type && strcmp(type, "transcript.done") == 0) snprintf(h->done, sizeof h->done, "%s", mc_json_string(msg, "text"));
    if (type && strcmp(type, "error") == 0) {
        snprintf(h->error_stage, sizeof h->error_stage, "%s", mc_json_string(msg, "stage"));
        snprintf(h->error_message, sizeof h->error_message, "%s", mc_json_string(msg, "message"));
    }
    json_object_put(msg);
    return 0;
}

static void *serve(void *arg) {
    int fd = *(int *)arg;
    sewn_peer peer = { .uid = 1000, .gid = 1000, .pid = 1, .known = true };
    sewn_serve_connection(&svc, fd, &peer);
    close(fd);
    return NULL;
}

static void write_json(int fd, const char *json) { mc_frame_write_fd(fd, MC_FRAME_JSON, (const unsigned char *)json, strlen(json)); }

/* Sends the start, `chunks` PCM frames of `chunk_bytes` each, then `last` (NULL: nothing), and reads to the end. */
static void run(struct heard *h, const char *start, int chunks, size_t chunk_bytes, const char *last) {
    int sv[2];
    MARY_ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    pthread_t thread;
    pthread_create(&thread, NULL, serve, &sv[1]);
    write_json(sv[0], start);
    unsigned char *pcm = calloc(1, chunk_bytes ? chunk_bytes : 1);
    for (int i = 0; i < chunks; i++) mc_frame_write_fd(sv[0], MC_FRAME_PCM, pcm, chunk_bytes);
    free(pcm);
    if (last) write_json(sv[0], last);
    else shutdown(sv[0], SHUT_WR);
    mc_frame_reader reader;
    mc_frame_reader_init(&reader, 0, false);
    while (mc_frame_reader_read_fd(&reader, sv[0], on_frame, h) == MC_IO_OK) {}
    mc_frame_reader_free(&reader);
    pthread_join(thread, NULL);
    close(sv[0]);
}

static int index_of(const struct heard *h, const char *type) {
    for (int i = 0; i < h->count; i++) if (strcmp(h->types[i], type) == 0) return i;
    return -1;
}

MARY_TEST(an_utterance_is_relayed_and_its_transcript_returned) {
    setup(true);
    struct heard h = { 0 };
    run(&h, "{\"type\":\"transcribe.start\"}", 2, 3200, "{\"type\":\"transcribe.end\"}");
    MARY_ASSERT_STR(vox.key, KEY);
    MARY_ASSERT_STR(vox.path, "/v1/audio/transcriptions/realtime?model=voxtral-mini-transcribe-realtime-2602");
    MARY_ASSERT_STR(vox.types[0], "session.update");
    MARY_ASSERT_EQ(vox.sample_rate, 16000);
    MARY_ASSERT_EQ(vox.appends, 2);
    MARY_ASSERT_EQ(vox.appended_bytes, 6400);
    MARY_ASSERT_STR(vox.types[vox.count - 1], "input_audio.end");
    MARY_ASSERT(vox.closed);
    MARY_ASSERT_EQ(index_of(&h, "transcribe.ready"), 0);
    MARY_ASSERT_STR(h.deltas, "what is the capital of France");
    MARY_ASSERT_STR(h.done, "What is the capital of France?");
    MARY_ASSERT_EQ(index_of(&h, "error"), -1);
    teardown();
}

MARY_TEST(a_large_frame_is_split_under_voxtrals_limit) {
    setup(true);
    struct heard h = { 0 };
    run(&h, "{\"type\":\"transcribe.start\",\"sample_rate\":48000}", 1, 300000, "{\"type\":\"transcribe.end\"}");
    MARY_ASSERT_EQ(vox.sample_rate, 48000);
    MARY_ASSERT_EQ(vox.appends, 2);
    MARY_ASSERT_EQ(vox.appended_bytes, 300000);
    teardown();
    setup(true);
    struct heard h2 = { 0 };
    run(&h2, "{\"type\":\"transcribe.start\",\"sample_rate\":12345}", 1, 320, "{\"type\":\"transcribe.end\"}");
    MARY_ASSERT_EQ(vox.sample_rate, 16000);   /* not a rate Voxtral takes */
    teardown();
}

MARY_TEST(a_flush_is_passed_on) {
    setup(true);
    int sv[2];
    MARY_ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    pthread_t thread;
    pthread_create(&thread, NULL, serve, &sv[1]);
    write_json(sv[0], "{\"type\":\"transcribe.start\"}");
    write_json(sv[0], "{\"type\":\"transcribe.flush\"}");
    write_json(sv[0], "{\"type\":\"transcribe.end\"}");
    struct heard h = { 0 };
    mc_frame_reader reader;
    mc_frame_reader_init(&reader, 0, false);
    while (mc_frame_reader_read_fd(&reader, sv[0], on_frame, &h) == MC_IO_OK) {}
    mc_frame_reader_free(&reader);
    pthread_join(thread, NULL);
    close(sv[0]);
    MARY_ASSERT_STR(vox.types[1], "input_audio.flush");
    teardown();
}

MARY_TEST(cancel_abandons_the_utterance) {
    setup(true);
    struct heard h = { 0 };
    run(&h, "{\"type\":\"transcribe.start\"}", 1, 3200, "{\"type\":\"cancel\"}");
    MARY_ASSERT(vox.closed);
    MARY_ASSERT_EQ(index_of(&h, "transcript.done"), -1);
    MARY_ASSERT_EQ(index_of(&h, "error"), -1);
    for (int i = 0; i < vox.count; i++) if (strcmp(vox.types[i], "input_audio.end") == 0) MARY_FAIL("an end was sent after a cancel");
    teardown();
}

MARY_TEST(a_client_that_hangs_up_abandons_it_too) {
    setup(true);
    struct heard h = { 0 };
    run(&h, "{\"type\":\"transcribe.start\"}", 1, 3200, NULL);
    MARY_ASSERT(vox.closed);
    MARY_ASSERT_EQ(index_of(&h, "error"), -1);
    teardown();
}

MARY_TEST(voxtrals_errors_and_silence_are_reported) {
    setup(true);
    vox.error_on_end = true;
    struct heard h = { 0 };
    run(&h, "{\"type\":\"transcribe.start\"}", 1, 3200, "{\"type\":\"transcribe.end\"}");
    MARY_ASSERT_STR(h.error_stage, "transcribe");
    MARY_ASSERT_STR(h.error_message, "Invalid audio");
    teardown();
    setup(true);
    vox.never_done = true;
    struct heard quiet = { 0 };
    run(&quiet, "{\"type\":\"transcribe.start\"}", 1, 3200, "{\"type\":\"transcribe.end\"}");
    MARY_ASSERT_STR(quiet.error_message, "Voxtral did not finish the transcript in time");
    teardown();
}

MARY_TEST(no_key_or_no_voxtral_means_no_session) {
    setup(false);
    struct heard h = { 0 };
    run(&h, "{\"type\":\"transcribe.start\"}", 0, 0, "{\"type\":\"transcribe.end\"}");
    MARY_ASSERT_STR(h.error_stage, "key");
    MARY_ASSERT_STR(vox.path, "");
    teardown();
    setup(true);
    vox.fail_open = true;
    struct heard refused = { 0 };
    run(&refused, "{\"type\":\"transcribe.start\"}", 0, 0, "{\"type\":\"transcribe.end\"}");
    MARY_ASSERT_STR(refused.error_stage, "network");
    MARY_ASSERT_STR(refused.error_message, "could not reach Voxtral: refused");
    teardown();
}

int main(void) {
    MARY_RUN(an_utterance_is_relayed_and_its_transcript_returned);
    MARY_RUN(a_large_frame_is_split_under_voxtrals_limit);
    MARY_RUN(a_flush_is_passed_on);
    MARY_RUN(cancel_abandons_the_utterance);
    MARY_RUN(a_client_that_hangs_up_abandons_it_too);
    MARY_RUN(voxtrals_errors_and_silence_are_reported);
    MARY_RUN(no_key_or_no_voxtral_means_no_session);
    MARY_TEST_MAIN_END();
}
