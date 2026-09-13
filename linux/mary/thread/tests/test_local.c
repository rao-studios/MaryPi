/* local.sock: the JSON ops, served over a socketpair the way threadd serves a connection. */
#include "test_support.h"

#include <pthread.h>
#include <sys/socket.h>

#include "common/io.h"
#include "common/lines.h"
#include "thread/local.h"

static thread_caller caller;

static void *serve(void *arg) {
    int fd = *(int *)arg;
    thread_local_serve(fd, &caller);
    close(fd);
    return NULL;
}

static struct json_object *replies[8];
static size_t n_replies;
static int take(const char *line, size_t len, void *user) {
    if (n_replies < 8) replies[n_replies++] = mc_json_parse(line, len);
    return 0;
}

/* Sends every line, closes, and collects what came back. */
static void exchange(const char *lines) {
    for (size_t i = 0; i < n_replies; i++) json_object_put(replies[i]);
    n_replies = 0;
    int sv[2];
    MARY_ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    pthread_t thread;
    pthread_create(&thread, NULL, serve, &sv[1]);
    MARY_ASSERT_EQ(mc_write_all(sv[0], lines, strlen(lines)), 0);
    shutdown(sv[0], SHUT_WR);
    mc_line_reader reader;
    mc_line_reader_init(&reader, 1 << 20);
    char buf[4096];
    for (;;) {
        ssize_t got = read(sv[0], buf, sizeof buf);
        if (got <= 0) break;
        mc_line_reader_feed(&reader, buf, (size_t)got, take, NULL);
    }
    mc_line_reader_free(&reader);
    close(sv[0]);
    pthread_join(thread, NULL);
}

MARY_TEST(the_local_socket_deposits_searches_and_refuses_the_unknown) {
    thread_store *s = open_store(true);
    caller = (thread_caller){ .store = s, .owner = "rao" };
    exchange("{\"type\":\"deposit\",\"group\":\"memory-rao\",\"label\":\"Memory\",\"text\":\"Paris is the capital of France.\",\"document_id\":\"m1\"}\n"
             "not json\n"
             "{\"type\":\"dance\"}\n"
             "{\"type\":\"stats\"}\n");
    MARY_ASSERT_EQ(n_replies, 4);
    MARY_ASSERT_STR(mc_json_type(replies[0]), "deposit.result");
    MARY_ASSERT_STR(mc_json_string(replies[0], "document_id"), "m1");
    MARY_ASSERT_STR(mc_json_string(replies[0], "family"), "memory");
    MARY_ASSERT_STR(mc_json_type(replies[1]), "error");
    MARY_ASSERT_STR(mc_json_type(replies[2]), "error");
    MARY_ASSERT_STR(mc_json_string(replies[2], "code"), "EINVAL");
    int64_t docs = 0;
    MARY_ASSERT(mc_json_int64(replies[3], "documents", &docs) && docs == 1);

    MARY_ASSERT(thread_store_enrich_drain(s) >= 1);
    exchange("{\"type\":\"search\",\"query\":\"capital of France\",\"lanes\":[\"personal\"]}\n"
             "{\"type\":\"search\",\"query\":\"capital of France\",\"lanes\":[\"conversation\"]}\n"
             "{\"type\":\"search\",\"query\":\"capital\",\"lanes\":[\"elsewhere\"]}\n");
    MARY_ASSERT_EQ(n_replies, 3);
    MARY_ASSERT_STR(mc_json_type(replies[0]), "search.result");
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(replies[0], "results")), 1);
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(mc_json_array(replies[0], "results"), 0), "lane"), "personal");
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(replies[1], "results")), 0);   /* the lane keeps it out */
    MARY_ASSERT_STR(mc_json_type(replies[2]), "error");

    exchange("{\"type\":\"library\"}\n{\"type\":\"schemas\"}\n{\"type\":\"ledger\",\"kind\":\"search\"}\n{\"type\":\"graph\",\"documents\":true}\n");
    MARY_ASSERT_EQ(n_replies, 4);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(replies[0], "groups")), 1);
    MARY_ASSERT(json_object_array_length(mc_json_array(replies[1], "families")) >= 10);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(replies[2], "rows")), 2);      /* the two searches that ran */
    MARY_ASSERT(json_object_array_length(mc_json_array(replies[3], "entities")) > 0);
    close_store(s);
}

MARY_TEST(the_trusted_caller_writes_for_another_owner_and_a_plain_one_cannot) {
    thread_store *s = open_store(false);
    caller = (thread_caller){ .store = s, .owner = "sewn", .trusted = true };
    exchange("{\"type\":\"deposit\",\"owner_id\":\"rao\",\"group\":\"memory-rao\",\"text\":\"a memory written by sewnd\",\"document_id\":\"m2\",\"source\":\"sewnd\"}\n");
    MARY_ASSERT_STR(mc_json_type(replies[0]), "deposit.result");
    caller = (thread_caller){ .store = s, .owner = "guest" };
    exchange("{\"type\":\"deposit\",\"owner_id\":\"rao\",\"group\":\"memory-rao\",\"text\":\"an intruder\",\"document_id\":\"m2\"}\n"
             "{\"type\":\"library\",\"owner_id\":\"rao\"}\n");
    MARY_ASSERT_STR(mc_json_type(replies[0]), "error");
    MARY_ASSERT_STR(mc_json_string(replies[0], "code"), "EPERM");
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(replies[1], "groups")), 0);   /* owner_id ignored: guest sees nothing */
    caller = (thread_caller){ .store = s, .owner = "rao" };
    exchange("{\"type\":\"documents\",\"ids\":[\"m2\"]}\n");
    struct json_object *doc = json_object_array_get_idx(mc_json_array(replies[0], "documents"), 0);
    MARY_ASSERT(doc != NULL);
    MARY_ASSERT_STR(mc_json_string(doc, "owner_id"), "rao");
    close_store(s);
}

int main(void) {
    MARY_RUN(the_local_socket_deposits_searches_and_refuses_the_unknown);
    MARY_RUN(the_trusted_caller_writes_for_another_owner_and_a_plain_one_cannot);
    MARY_TEST_MAIN_END();
}
