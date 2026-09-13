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
#include "thread/embedder.h"

/* A fake sewnd: answers `calls` connections. Each embed gets deterministic vectors
 * (the i-th text's vector is all i + 1); the answer is scripted by `mode`. */
static char dir[64], sock[128];
static int mode;                 /* 0 ok, 1 error 429, 2 error key, 3 wrong count */
static int calls_served, texts_seen;

static struct json_object *got;
static int take(uint8_t kind, const unsigned char *bytes, size_t len, void *user) {
    if (kind == MC_FRAME_JSON) got = mc_json_parse((const char *)bytes, len);
    return 1;
}

static void *serve(void *arg) {
    int listener = *(int *)arg;
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) break;
        mc_frame_reader reader;
        mc_frame_reader_init(&reader, 0, false);
        got = NULL;
        int st;
        while ((st = mc_frame_reader_read_fd(&reader, fd, take, NULL)) == MC_IO_OK) {}
        mc_frame_reader_free(&reader);
        struct json_object *reply = json_object_new_object();
        struct json_object *texts = mc_json_array(got, "texts");
        size_t n = texts ? json_object_array_length(texts) : 0;
        texts_seen += (int)n;
        calls_served++;
        if (mode == 1 || mode == 2) {
            json_object_object_add(reply, "type", json_object_new_string("error"));
            json_object_object_add(reply, "stage", json_object_new_string(mode == 2 ? "key" : "network"));
            json_object_object_add(reply, "message", json_object_new_string(mode == 2 ? "no key is set" : "rate-limited"));
            if (mode == 1) json_object_object_add(reply, "status", json_object_new_int(429));
        } else if (strcmp(mc_json_type(got), "graph.extract") == 0) {
            json_object_object_add(reply, "type", json_object_new_string("graph.extract.result"));
            json_object_object_add(reply, "json", json_object_new_string("{\"entities\":[{\"name\":\"Ada\",\"kind\":\"person\"}],\"relationships\":[]}"));
        } else {
            json_object_object_add(reply, "type", json_object_new_string("embed.result"));
            json_object_object_add(reply, "dim", json_object_new_int(4));
            struct json_object *vectors = json_object_new_array();
            size_t count = mode == 3 ? n + 1 : n;
            for (size_t i = 0; i < count; i++) {
                float v[4] = { (float)i + 1, (float)i + 1, (float)i + 1, (float)i + 1 };
                char *b64 = mc_base64_encode_alloc((const unsigned char *)v, sizeof v, NULL);
                json_object_array_add(vectors, json_object_new_string(b64));
                free(b64);
            }
            json_object_object_add(reply, "vectors_b64", vectors);
        }
        mc_frame_write_json(fd, reply);
        json_object_put(reply);
        if (got) json_object_put(got);
        close(fd);
    }
    return NULL;
}

MARY_TEST(embeddings_come_back_as_floats_in_batches_and_errors_are_typed) {
    snprintf(dir, sizeof dir, "/tmp/thread-embed-XXXXXX");
    MARY_ASSERT(mkdtemp(dir) != NULL);
    snprintf(sock, sizeof sock, "%s/sewn.sock", dir);
    int listener = mc_listen_unix(sock, 0600);
    MARY_ASSERT(listener >= 0);
    pthread_t t;
    pthread_create(&t, NULL, serve, &listener);

    thread_embedder e;
    MARY_ASSERT_EQ(thread_sewn_embedder_init(&e, sock), 0);
    e.dim = 4;
    e.batch_max = 64;
    const char *texts[100];
    for (int i = 0; i < 100; i++) texts[i] = "a text";
    float *out = calloc(100 * 4, sizeof *out);
    char message[200] = "";
    MARY_ASSERT_EQ(thread_embedder_embed(&e, texts, 100, out, message, sizeof message), 0);
    MARY_ASSERT_EQ(calls_served, 2);                    /* 64 + 36 */
    MARY_ASSERT_EQ(texts_seen, 100);
    MARY_ASSERT_NEAR(out[0], 1, 1e-6);
    MARY_ASSERT_NEAR(out[63 * 4 + 3], 64, 1e-6);
    MARY_ASSERT_NEAR(out[64 * 4], 1, 1e-6);            /* the second batch starts over */
    mode = 1;
    MARY_ASSERT_EQ(thread_embedder_embed(&e, texts, 1, out, message, sizeof message), -EAGAIN);
    MARY_ASSERT_STR(message, "rate-limited");
    mode = 2;
    MARY_ASSERT_EQ(thread_embedder_embed(&e, texts, 1, out, message, sizeof message), -ENOENT);
    mode = 3;
    MARY_ASSERT_EQ(thread_embedder_embed(&e, texts, 1, out, message, sizeof message), -EIO);
    mode = 0;
    char *json = NULL;
    MARY_ASSERT_EQ(e.extract(texts, 1, "prompt", &json, message, sizeof message, e.user), 0);
    MARY_ASSERT(json && strstr(json, "Ada"));
    free(json);
    free(out);
    thread_sewn_embedder_free(&e);
    thread_embedder none = { 0 };
    MARY_ASSERT_EQ(thread_embedder_embed(&none, texts, 1, NULL, message, sizeof message), -ENOSYS);
    shutdown(listener, SHUT_RDWR);   /* Linux: close alone does not wake a blocked accept */
    close(listener);
    pthread_join(t, NULL);
    unlink(sock);
    rmdir(dir);
    thread_embedder gone;
    thread_sewn_embedder_init(&gone, sock);
    MARY_ASSERT_EQ(thread_embedder_embed(&gone, texts, 1, out = calloc(4, sizeof *out), message, sizeof message), -EIO);
    free(out);
    thread_sewn_embedder_free(&gone);
}

int main(void) {
    mc_ignore_sigpipe();
    MARY_RUN(embeddings_come_back_as_floats_in_batches_and_errors_are_typed);
    MARY_TEST_MAIN_END();
}
