#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/frame.h"
#include "common/io.h"
#include "common/json.h"
#include "common/service.h"
#include "mary_test.h"

/* A fake daemon on the other end of a socketpair: reads one JSON frame, answers
 * a PCM frame (which a client skips) and then {"type":"echo","got":<type>}. */
static int on_frame(uint8_t kind, const unsigned char *bytes, size_t len, void *user) {
    struct json_object **req = user;
    if (kind == MC_FRAME_JSON) *req = mc_json_parse((const char *)bytes, len);
    return 1;
}

static void *serve(void *arg) {
    int fd = *(int *)arg;
    mc_frame_reader reader;
    mc_frame_reader_init(&reader, 0, false);
    struct json_object *req = NULL;
    int status;
    while ((status = mc_frame_reader_read_fd(&reader, fd, on_frame, &req)) == MC_IO_OK) {}
    mc_frame_reader_free(&reader);
    float pcm[4] = { 0, 0.5f, -0.5f, 1 };
    mc_frame_write_fd(fd, MC_FRAME_PCM, (const unsigned char *)pcm, sizeof pcm);
    struct json_object *reply = json_object_new_object();
    json_object_object_add(reply, "type", json_object_new_string("echo"));
    json_object_object_add(reply, "got", json_object_new_string(req ? mc_json_string(req, "type") : "?"));
    mc_frame_write_json(fd, reply);
    json_object_put(reply);
    if (req) json_object_put(req);
    close(fd);
    return NULL;
}

MARY_TEST(a_call_skips_binary_frames_and_returns_the_first_json_reply) {
    int pair[2];
    MARY_ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    pthread_t t;
    pthread_create(&t, NULL, serve, &pair[1]);
    struct json_object *req = json_object_new_object();
    json_object_object_add(req, "type", json_object_new_string("embed"));
    struct json_object *reply = NULL;
    MARY_ASSERT_EQ(mc_service_call(pair[0], req, &reply), 0);
    MARY_ASSERT(reply != NULL);
    if (reply) {
        MARY_ASSERT_STR(mc_json_string(reply, "type"), "echo");
        MARY_ASSERT_STR(mc_json_string(reply, "got"), "embed");
        json_object_put(reply);
    }
    json_object_put(req);
    close(pair[0]);
    pthread_join(t, NULL);
}

MARY_TEST(a_closed_peer_is_a_reset_and_a_missing_socket_an_errno) {
    int pair[2];
    MARY_ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    close(pair[1]);
    struct json_object *reply = NULL;
    MARY_ASSERT_EQ(mc_service_read_reply(pair[0], &reply), -ECONNRESET);
    MARY_ASSERT(reply == NULL);
    close(pair[0]);
    int rc = mc_service_connect("/tmp/no-such-dir-for-mary/sewn.sock");
    MARY_ASSERT(rc == -ENOENT || rc == -ECONNREFUSED);
    MARY_ASSERT_EQ(mc_service_connect("/tmp/" "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), -ENAMETOOLONG);
}

MARY_TEST(the_default_socket_honours_the_environment) {
    setenv("MARY_TEST_SOCKET", "/tmp/x.sock", 1);
    MARY_ASSERT_STR(mc_service_default_socket("MARY_TEST_SOCKET", "/run/x"), "/tmp/x.sock");
    setenv("MARY_TEST_SOCKET", "", 1);
    MARY_ASSERT_STR(mc_service_default_socket("MARY_TEST_SOCKET", "/run/x"), "/run/x");
    MARY_ASSERT_STR(mc_service_default_socket(NULL, "/run/y"), "/run/y");
}

int main(void) {
    mc_ignore_sigpipe();
    MARY_RUN(a_call_skips_binary_frames_and_returns_the_first_json_reply);
    MARY_RUN(a_closed_peer_is_a_reset_and_a_missing_socket_an_errno);
    MARY_RUN(the_default_socket_honours_the_environment);
    MARY_TEST_MAIN_END();
}
