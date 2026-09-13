#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "conduit/grpc.h"
#include "conduit/thread.pb-c.h"
#include "mary_test.h"

static void echo(const uint8_t *request, size_t len, conduit_reply *reply, void *user) {
    reply->body = malloc(len ? len : 1);
    memcpy(reply->body, request, len);
    reply->body_len = len;
}

static void refuse(const uint8_t *request, size_t len, conduit_reply *reply, void *user) {
    reply->status = CONDUIT_PERMISSION_DENIED;
    snprintf(reply->message, sizeof reply->message, "not yours: 100%% sure");
}

static const conduit_route ROUTES[] = {
    { "/conduit.test/Echo", echo },
    { "/conduit.test/Refuse", refuse },
};

static void *serve(void *arg) {
    int fd = *(int *)arg;
    conduit_serve(fd, ROUTES, 2, NULL);
    close(fd);
    return NULL;
}

/* One call on a fresh socketpair with a server thread behind it. */
static int call(const char *path, const uint8_t *request, size_t len, conduit_result *result) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return -errno;
    pthread_t thread;
    pthread_create(&thread, NULL, serve, &sv[1]);
    int rc = conduit_call(sv[0], path, request, len, 5000, result);
    close(sv[0]);
    pthread_join(thread, NULL);
    return rc;
}

MARY_TEST(a_unary_call_round_trips) {
    conduit_result r;
    MARY_ASSERT_EQ(call("/conduit.test/Echo", (const uint8_t *)"hello thread", 12, &r), 0);
    MARY_ASSERT_EQ(r.status, CONDUIT_OK);
    MARY_ASSERT_EQ(r.body_len, 12);
    MARY_ASSERT(memcmp(r.body, "hello thread", 12) == 0);
    free(r.body);
    MARY_ASSERT_EQ(call("/conduit.test/Echo", NULL, 0, &r), 0);   /* an empty message is a message */
    MARY_ASSERT_EQ(r.status, CONDUIT_OK);
    MARY_ASSERT_EQ(r.body_len, 0);
    free(r.body);
}

MARY_TEST(statuses_and_messages_travel_in_the_trailers) {
    conduit_result r;
    MARY_ASSERT_EQ(call("/thread.v1.ThreadRegistration/Session", (const uint8_t *)"x", 1, &r), 0);
    MARY_ASSERT_EQ(r.status, CONDUIT_UNIMPLEMENTED);
    MARY_ASSERT_STR(r.message, "unknown method /thread.v1.ThreadRegistration/Session");
    MARY_ASSERT(r.body == NULL);
    MARY_ASSERT_EQ(call("/conduit.test/Refuse", (const uint8_t *)"x", 1, &r), 0);
    MARY_ASSERT_EQ(r.status, CONDUIT_PERMISSION_DENIED);
    MARY_ASSERT_STR(r.message, "not yours: 100% sure");      /* percent-encoded on the wire */
}

MARY_TEST(an_oversize_request_is_refused_before_it_is_sent) {
    conduit_result r;
    uint8_t *big = calloc(1, CONDUIT_MESSAGE_MAX + 1);
    int sv[2];
    MARY_ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    MARY_ASSERT_EQ(conduit_call(sv[0], "/conduit.test/Echo", big, CONDUIT_MESSAGE_MAX + 1, 1000, &r), -EMSGSIZE);
    close(sv[0]);
    close(sv[1]);
    free(big);
}

MARY_TEST(thread_v1_messages_pack_and_unpack) {
    Thread__V1__ThreadIndexItem item = THREAD__V1__THREAD_INDEX_ITEM__INIT;
    char *texts[] = { "What is the capital of France?", "Paris." };
    item.document_id = "mary-turn-1757700000000-ab12";
    item.n_texts = 2;
    item.texts = texts;
    item.media_type = "text";
    Thread__V1__ThreadIndexItem *items[] = { &item };
    Thread__V1__ThreadIndexRequest request = THREAD__V1__THREAD_INDEX_REQUEST__INIT;
    request.owner_id = "mary";
    request.group_id = "mary-conversations";
    request.scope = "personal";
    request.n_items = 1;
    request.items = items;

    size_t len = thread__v1__thread_index_request__get_packed_size(&request);
    uint8_t *packed = malloc(len);
    thread__v1__thread_index_request__pack(&request, packed);
    conduit_result r;
    MARY_ASSERT_EQ(call("/conduit.test/Echo", packed, len, &r), 0);
    Thread__V1__ThreadIndexRequest *back = thread__v1__thread_index_request__unpack(NULL, r.body_len, r.body);
    MARY_ASSERT(back != NULL);
    MARY_ASSERT_STR(back->group_id, "mary-conversations");
    MARY_ASSERT_EQ(back->n_items, 1);
    MARY_ASSERT_STR(back->items[0]->texts[1], "Paris.");
    thread__v1__thread_index_request__free_unpacked(back, NULL);
    free(r.body);
    free(packed);
}

/* Real gRPC on the other end: Python's grpcio (C core) against conduit_serve. */
MARY_TEST(grpcio_speaks_to_conduit) {
    if (system("python3 -c 'import grpc' > /dev/null 2>&1") != 0) {
        printf("     (grpcio is not installed here; skipped)\n");
        return;
    }
    char dir[] = "/tmp/conduit-XXXXXX", path[64];   /* it must fit sun_path */
    MARY_ASSERT(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/grpc.sock", dir);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    MARY_ASSERT_EQ(bind(listener, (struct sockaddr *)&addr, sizeof addr), 0);
    MARY_ASSERT_EQ(listen(listener, 4), 0);
    pid_t child = fork();
    if (child == 0) {
        for (;;) {
            int fd = accept(listener, NULL, NULL);
            if (fd < 0) _exit(0);
            conduit_serve(fd, ROUTES, 2, NULL);
            close(fd);
        }
    }
    close(listener);
    char script[1536];
    snprintf(script, sizeof script,
        "python3 - '%s' <<'PY'\n"
        "import grpc, sys\n"
        "ident = lambda b: b\n"
        "ch = grpc.insecure_channel('unix:' + sys.argv[1])\n"
        "echo = ch.unary_unary('/conduit.test/Echo', request_serializer=ident, response_deserializer=ident)\n"
        "print('echo', echo(b'hello thread', timeout=5).decode())\n"
        "for name, call, payload in (('unknown', ch.unary_unary('/nope.v1.Nope/Nope', request_serializer=ident, response_deserializer=ident), b'x'),\n"
        "                            ('refuse', ch.unary_unary('/conduit.test/Refuse', request_serializer=ident, response_deserializer=ident), b'x'),\n"
        "                            ('big', echo, b'x' * (5 << 20))):\n"
        "    try:\n"
        "        call(payload, timeout=5)\n"
        "        print(name, 'ok')\n"
        "    except grpc.RpcError as e:\n"
        "        print(name, e.code().value[0], e.details())\n"
        "PY\n", path);
    FILE *out = popen(script, "r");
    char output[2048] = "";
    size_t n = fread(output, 1, sizeof output - 1, out);
    output[n] = 0;
    pclose(out);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    unlink(path);
    rmdir(dir);
    MARY_ASSERT(strstr(output, "echo hello thread\n") != NULL);
    MARY_ASSERT(strstr(output, "unknown 12 unknown method /nope.v1.Nope/Nope\n") != NULL);
    MARY_ASSERT(strstr(output, "refuse 7 not yours: 100% sure\n") != NULL);
    MARY_ASSERT(strstr(output, "big 8") != NULL);
    if (mary_test_case_failed) fprintf(stderr, "  python said:\n%s", output);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    MARY_RUN(a_unary_call_round_trips);
    MARY_RUN(statuses_and_messages_travel_in_the_trailers);
    MARY_RUN(an_oversize_request_is_refused_before_it_is_sent);
    MARY_RUN(thread_v1_messages_pack_and_unpack);
    MARY_RUN(grpcio_speaks_to_conduit);
    MARY_TEST_MAIN_END();
}
