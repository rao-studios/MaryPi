#if defined(__APPLE__)
#define _DARWIN_C_SOURCE    /* mkdtemp, pread under _POSIX_C_SOURCE */
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/frame.h"
#include "common/json.h"
#include "mary_test.h"
#include "sewn/client.h"
#include "sewn/server.h"

static const char *KEY = "abcdEFGH1234ijklMNOP5678qrst";
static int verdict = 1;
static char dir[64];
static sewn_service svc;

static int stub_verify(const char *key, char *message, size_t cap, void *user) {
    if (strcmp(key, KEY) != 0) MARY_FAIL("verify was handed another key");
    snprintf(message, cap, "%s", verdict > 0 ? "Mistral accepted the key" : verdict == 0 ? "Mistral rejected the key" : "offline");
    return verdict;
}

static bool uid_1000_administers(uid_t uid, const char *group) { return uid == 1000; }

static void setup(void) {
    snprintf(dir, sizeof dir, "/tmp/sewn-server-XXXXXX");
    MARY_ASSERT(mkdtemp(dir) != NULL);
    sewn_service_init(&svc, dir);
    svc.in_group = uid_1000_administers;
    svc.verify = stub_verify;
    verdict = 1;
}

static void teardown(void) {
    sewn_service_free(&svc);
    char path[128];
    snprintf(path, sizeof path, "%s/mistral.key", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/calls.jsonl", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/mistral.verified", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/sewn.sock", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/plain", dir);
    unlink(path);
    rmdir(dir);
}

/* One request, served in this thread: the socket buffers it until the server reads. */
static struct json_object *ask(uid_t uid, const char *request) {
    int sv[2];
    MARY_ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    MARY_ASSERT_EQ(mc_frame_write_fd(sv[0], MC_FRAME_JSON, (const unsigned char *)request, strlen(request)), 0);
    sewn_peer peer = { .uid = uid, .gid = uid, .pid = 1, .known = true };
    sewn_serve_connection(&svc, sv[1], &peer);
    close(sv[1]);
    struct json_object *reply = NULL;
    MARY_ASSERT_EQ(sewn_read_reply(sv[0], &reply), 0);
    close(sv[0]);
    return reply;
}

static void set_request(char *out, size_t cap, const char *key) {
    snprintf(out, cap, "{\"type\":\"key.set\",\"key\":\"%s\"}", key);
}

MARY_TEST(status_before_any_key) {
    setup();
    struct json_object *r = ask(1001, "{\"type\":\"key.status\"}");
    bool present = true;
    MARY_ASSERT_STR(mc_json_type(r), "key.status");
    MARY_ASSERT(mc_json_bool(r, "present", &present) && !present);
    MARY_ASSERT(!mc_json_int64(r, "verified_at", NULL));
    json_object_put(r);
    teardown();
}

MARY_TEST(setting_the_key_needs_the_admin_group) {
    setup();
    char request[128];
    set_request(request, sizeof request, KEY);
    struct json_object *r = ask(1001, request);
    MARY_ASSERT_STR(mc_json_type(r), "error");
    MARY_ASSERT_STR(mc_json_string(r, "stage"), "auth");
    json_object_put(r);
    MARY_ASSERT(!sewn_key_store_present(&svc.keys));
    r = ask(1000, request);
    bool present = false;
    MARY_ASSERT_STR(mc_json_type(r), "key.status");
    MARY_ASSERT(mc_json_bool(r, "present", &present) && present);
    json_object_put(r);
    teardown();
}

MARY_TEST(a_bad_key_is_refused_without_storing_it) {
    setup();
    struct json_object *r = ask(1000, "{\"type\":\"key.set\",\"key\":\"nope\"}");
    MARY_ASSERT_STR(mc_json_string(r, "stage"), "key");
    json_object_put(r);
    r = ask(1000, "{\"type\":\"key.set\"}");
    MARY_ASSERT_STR(mc_json_string(r, "stage"), "key");
    json_object_put(r);
    MARY_ASSERT(!sewn_key_store_present(&svc.keys));
    teardown();
}

MARY_TEST(verify_records_only_an_accepted_key) {
    setup();
    struct json_object *r = ask(1001, "{\"type\":\"key.verify\"}");
    MARY_ASSERT_STR(mc_json_string(r, "stage"), "key");   /* nothing stored yet */
    json_object_put(r);

    char request[128];
    set_request(request, sizeof request, KEY);
    json_object_put(ask(1000, request));

    verdict = 0;
    r = ask(1001, "{\"type\":\"key.verify\"}");
    bool ok = true;
    MARY_ASSERT(mc_json_bool(r, "ok", &ok) && !ok);
    MARY_ASSERT_STR(mc_json_string(r, "message"), "Mistral rejected the key");
    MARY_ASSERT(!mc_json_int64(r, "verified_at", NULL));
    json_object_put(r);

    verdict = -EIO;
    r = ask(1001, "{\"type\":\"key.verify\"}");
    MARY_ASSERT_STR(mc_json_string(r, "stage"), "network");
    MARY_ASSERT_STR(mc_json_string(r, "message"), "offline");
    json_object_put(r);

    verdict = 1;
    r = ask(1001, "{\"type\":\"key.verify\"}");
    int64_t at = 0;
    MARY_ASSERT(mc_json_bool(r, "ok", &ok) && ok);
    MARY_ASSERT(mc_json_int64(r, "verified_at", &at) && at > 1700000000000LL);
    json_object_put(r);
    teardown();
}

MARY_TEST(unknown_and_malformed_requests_get_errors) {
    setup();
    struct json_object *r = ask(1001, "{\"type\":\"launch.rockets\"}");
    MARY_ASSERT_STR(mc_json_string(r, "stage"), "request");
    json_object_put(r);
    r = ask(1001, "[1,2]");
    MARY_ASSERT_STR(mc_json_string(r, "stage"), "request");
    json_object_put(r);
    r = ask(1001, "{\"type\":\"turn.start\"}");
    MARY_ASSERT_STR(mc_json_string(r, "message"), "turn.start needs a request with messages");
    json_object_put(r);
    r = ask(1001, "{\"type\":\"transcribe.start\"}");
    MARY_ASSERT_STR(mc_json_string(r, "stage"), svc.ws ? "key" : "network");   /* no key stored, or no libwebsockets */
    json_object_put(r);
    teardown();
}

MARY_TEST(the_key_never_reaches_the_log) {
    setup();
    char log_path[] = "/tmp/sewn-log-XXXXXX";
    int log_fd = mkstemp(log_path);
    MARY_ASSERT(log_fd >= 0);
    fflush(stderr);
    int saved = dup(2);
    dup2(log_fd, 2);
    char request[128];
    set_request(request, sizeof request, KEY);
    json_object_put(ask(1000, request));
    json_object_put(ask(1001, "{\"type\":\"key.verify\"}"));
    json_object_put(ask(1001, "{\"type\":\"key.status\"}"));
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    char text[8192] = "";
    ssize_t n = pread(log_fd, text, sizeof text - 1, 0);
    text[n > 0 ? n : 0] = 0;
    MARY_ASSERT(strstr(text, "key.set from uid 1000") != NULL);   /* the log did run */
    MARY_ASSERT(strstr(text, KEY) == NULL);
    close(log_fd);
    unlink(log_path);
    teardown();
}

struct serving {
    int fd;
    uid_t uid;
};

static void *serve_one(void *arg) {
    struct serving *s = arg;
    sewn_peer peer = { .uid = s->uid, .gid = s->uid, .pid = 1, .known = true };
    sewn_serve_connection(&svc, s->fd, &peer);
    close(s->fd);
    return NULL;
}

/* MARK: - The ops threadd and maryd call */

static char last_path[128], last_body[8192];
static int transport_calls;

/* A Mistral that answers embeddings and completions whole. */
static int scripted_post(const char *path, const char *key, const char *body, size_t body_len, sewn_bytes_fn on_bytes, sewn_stop_fn should_stop,
                         void *user, long *status, char *message, size_t cap, void *transport_user) {
    transport_calls++;
    if (strcmp(key, KEY) != 0) MARY_FAIL("the transport was handed another key");
    snprintf(last_path, sizeof last_path, "%s", path);
    snprintf(last_body, sizeof last_body, "%.*s", (int)body_len, body);
    const char *answer;
    if (strcmp(path, "/v1/embeddings") == 0) {
        answer = "{\"data\":[{\"index\":0,\"embedding\":[1,0]},{\"index\":1,\"embedding\":[0,1]}]}";
    } else if (strstr(last_body, "\"tools\"")) {
        answer = "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"id\":\"c1\",\"function\":{\"name\":\"bring_window_forward\",\"arguments\":\"{\\\"app\\\":\\\"Finder\\\"}\"}}]}}]}";
    } else if (strstr(last_body, "summarization assistant")) {
        answer = "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"**Bread**\\n\\nThe user wanted bread from the bakery downstairs.\"}}]}";
    } else {
        answer = "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"{\\\"entities\\\":[{\\\"name\\\":\\\"Ada\\\",\\\"kind\\\":\\\"person\\\"}],\\\"relationships\\\":[]}\"}}]}";
    }
    *status = 200;
    on_bytes(answer, strlen(answer), 200, user);
    return 0;
}

MARY_TEST(embed_extract_summarize_and_complete_answer_and_are_ledgered) {
    setup();
    svc.post_stream = scripted_post;
    svc.retrieve = NULL;
    transport_calls = 0;
    char request[128];
    set_request(request, sizeof request, KEY);
    json_object_put(ask(1000, request));

    struct json_object *r = ask(1001, "{\"type\":\"embed\",\"texts\":[\"a\",\"b\"],\"purpose\":\"index\"}");
    MARY_ASSERT_STR(mc_json_type(r), "embed.result");
    int64_t dim = 0;
    MARY_ASSERT(mc_json_int64(r, "dim", &dim) && dim == 2);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(r, "vectors_b64")), 2);
    MARY_ASSERT_STR(mc_json_string(r, "model"), "mistral-embed");
    MARY_ASSERT_STR(last_path, "/v1/embeddings");
    MARY_ASSERT(strstr(last_body, "\"encoding_format\":\"float\"") != NULL);
    json_object_put(r);

    r = ask(1001, "{\"type\":\"graph.extract\",\"texts\":[\"Ada wrote a program.\"],\"prompt\":\"Extract entities.\"}");
    MARY_ASSERT_STR(mc_json_type(r), "graph.extract.result");
    MARY_ASSERT(strstr(mc_json_string(r, "json"), "Ada") != NULL);
    MARY_ASSERT(strstr(last_body, "\"model\":\"mistral-tiny\"") != NULL);
    MARY_ASSERT(strstr(last_body, "Extract entities.") != NULL);
    json_object_put(r);

    r = ask(1001, "{\"type\":\"summarize\",\"messages\":[{\"role\":\"user\",\"content\":\"I want bread\"}],\"recent\":\"from downstairs\"}");
    MARY_ASSERT_STR(mc_json_type(r), "summarize.result");
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(r, "lines")), 1);
    MARY_ASSERT(strstr(last_body, "[user]: I want bread\\n[user]: from downstairs") != NULL);
    json_object_put(r);

    r = ask(1001, "{\"type\":\"complete\",\"instructions\":\"Call tools by name.\",\"messages\":[{\"role\":\"user\",\"content\":\"bring Finder forward\"}],"
                  "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"bring_window_forward\"}}],\"temperature\":0}");
    MARY_ASSERT_STR(mc_json_type(r), "complete.result");
    struct json_object *calls = mc_json_array(r, "tool_calls");
    MARY_ASSERT_EQ(json_object_array_length(calls), 1);
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(calls, 0), "name"), "bring_window_forward");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(calls, 0), "arguments"), "{\"app\":\"Finder\"}");
    MARY_ASSERT(strstr(last_body, "\"model\":\"mistral-medium-latest\"") != NULL);
    MARY_ASSERT(strstr(last_body, "\"role\":\"system\",\"content\":\"Call tools by name.\"") != NULL);
    json_object_put(r);
    MARY_ASSERT_EQ(transport_calls, 4);

    r = ask(1001, "{\"type\":\"complete\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"provider\":\"tinker\"}");
    MARY_ASSERT_STR(mc_json_string(r, "stage"), "engine");
    MARY_ASSERT_STR(mc_json_string(r, "message"), "Thinking Machines is not available yet");
    json_object_put(r);
    MARY_ASSERT_EQ(transport_calls, 4);                    /* nothing was sent */
    r = ask(1001, "{\"type\":\"graph.extract\",\"texts\":[\"x\"],\"prompt\":\"p\",\"provider\":\"local\"}");
    MARY_ASSERT_STR(mc_json_string(r, "stage"), "engine");
    json_object_put(r);

    r = ask(1001, "{\"type\":\"calls.list\"}");
    MARY_ASSERT_STR(mc_json_type(r), "calls.list.result");
    struct json_object *rows = mc_json_array(r, "calls");
    MARY_ASSERT_EQ(json_object_array_length(rows), 4);      /* the four; the refused engine wrote nothing */
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(rows, 0), "purpose"), "skills");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(rows, 1), "purpose"), "summarize");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(rows, 2), "purpose"), "extract");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(rows, 3), "purpose"), "index");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(rows, 3), "host"), "api.mistral.ai");
    MARY_ASSERT(strstr(json_object_to_json_string(r), KEY) == NULL);
    MARY_ASSERT(strstr(json_object_to_json_string(r), "bread") == NULL);   /* never a body */
    json_object_put(r);
    r = ask(1001, "{\"type\":\"calls.stats\"}");
    int64_t count = 0;
    MARY_ASSERT(mc_json_int64(mc_json_object(r, "stats"), "count", &count) && count == 4);
    json_object_put(r);
    teardown();
}

MARY_TEST(the_client_sets_the_key_through_the_socket) {
    setup();
    int sv[2];
    MARY_ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    struct serving serving = { sv[1], 1000 };
    pthread_t thread;
    MARY_ASSERT_EQ(pthread_create(&thread, NULL, serve_one, &serving), 0);
    struct json_object *reply = NULL;
    MARY_ASSERT_EQ(sewn_call_key_set(sv[0], KEY, strlen(KEY), &reply), 0);
    pthread_join(thread, NULL);
    close(sv[0]);
    bool present = false;
    MARY_ASSERT(mc_json_bool(reply, "present", &present) && present);
    json_object_put(reply);
    MARY_ASSERT_EQ(sewn_call_key_set(-1, "bad key", 7, &reply), -EINVAL);
    teardown();
}

MARY_TEST(the_listener_replaces_only_a_stale_socket) {
    setup();
    char path[128], plain[128];
    snprintf(path, sizeof path, "%s/sewn.sock", dir);
    snprintf(plain, sizeof plain, "%s/plain", dir);
    int fd = sewn_listen(path, 0660);
    MARY_ASSERT(fd >= 0);
    struct stat st;
    MARY_ASSERT_EQ(stat(path, &st), 0);
    MARY_ASSERT_EQ(st.st_mode & 0777, 0660);
    close(fd);
    fd = sewn_listen(path, 0660);      /* the socket file from before is stale now */
    MARY_ASSERT(fd >= 0);
    int client = sewn_connect(path);
    MARY_ASSERT(client >= 0);
    close(client);
    close(fd);
    close(open(plain, O_CREAT | O_WRONLY, 0600));
    MARY_ASSERT_EQ(sewn_listen(plain, 0660), -EEXIST);
    teardown();
}

int main(void) {
    MARY_RUN(status_before_any_key);
    MARY_RUN(setting_the_key_needs_the_admin_group);
    MARY_RUN(a_bad_key_is_refused_without_storing_it);
    MARY_RUN(verify_records_only_an_accepted_key);
    MARY_RUN(unknown_and_malformed_requests_get_errors);
    MARY_RUN(the_key_never_reaches_the_log);
    MARY_RUN(embed_extract_summarize_and_complete_answer_and_are_ledgered);
    MARY_RUN(the_client_sets_the_key_through_the_socket);
    MARY_RUN(the_listener_replaces_only_a_stale_socket);
    MARY_TEST_MAIN_END();
}
