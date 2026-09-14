#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/io.h"
#include "common/json.h"
#include "conduit/grpc.h"
#include "thread/client.h"
#include "mary_test.h"

static char dir[64], sock[128];
static Thread__V1__ThreadIndexRequest *indexed;     /* the last Index request the fake threadd got */

static void on_index(const uint8_t *request, size_t len, conduit_reply *reply, void *user) {
    if (indexed) thread__v1__thread_index_request__free_unpacked(indexed, NULL);
    indexed = thread__v1__thread_index_request__unpack(NULL, len, request);
    Thread__V1__ThreadIndexResponse resp = THREAD__V1__THREAD_INDEX_RESPONSE__INIT;
    char *cids[1] = { indexed && indexed->n_items ? indexed->items[0]->document_id : "" };
    resp.success = indexed != NULL;
    resp.indexed_count = indexed ? (int)indexed->n_items : 0;
    resp.n_full_cids = 1;
    resp.full_cids = cids;
    reply->body_len = thread__v1__thread_index_response__get_packed_size(&resp);
    reply->body = malloc(reply->body_len);
    thread__v1__thread_index_response__pack(&resp, reply->body);
}

static void on_library(const uint8_t *request, size_t len, conduit_reply *reply, void *user) {
    Thread__V1__ThreadGroup group = THREAD__V1__THREAD_GROUP__INIT;
    group.id = "memory-rao";
    group.label = "Memory";
    group.owner_id = "rao";
    Thread__V1__ThreadGroup *groups[] = { &group };
    Thread__V1__ThreadLibraryResponse resp = THREAD__V1__THREAD_LIBRARY_RESPONSE__INIT;
    resp.n_groups = 1;
    resp.groups = groups;
    reply->body_len = thread__v1__thread_library_response__get_packed_size(&resp);
    reply->body = malloc(reply->body_len);
    thread__v1__thread_library_response__pack(&resp, reply->body);
}

static void on_documents(const uint8_t *request, size_t len, conduit_reply *reply, void *user) {
    Thread__V1__ThreadDocumentsRequest *req = thread__v1__thread_documents_request__unpack(NULL, len, request);
    char *texts[] = { "What is the capital of France?", "Paris." };
    Thread__V1__ThreadDocumentContent doc = THREAD__V1__THREAD_DOCUMENT_CONTENT__INIT;
    doc.id = req && req->n_document_ids ? req->document_ids[0] : "";
    doc.group_id = "memory-rao";
    doc.n_texts = 2;
    doc.texts = texts;
    Thread__V1__ThreadDocumentContent *docs[] = { &doc };
    Thread__V1__ThreadDocumentsResponse resp = THREAD__V1__THREAD_DOCUMENTS_RESPONSE__INIT;
    resp.n_documents = 1;
    resp.documents = docs;
    reply->body_len = thread__v1__thread_documents_response__get_packed_size(&resp);
    reply->body = malloc(reply->body_len);
    thread__v1__thread_documents_response__pack(&resp, reply->body);
    if (req) thread__v1__thread_documents_request__free_unpacked(req, NULL);
}

static const conduit_route routes[] = {
    { "/thread.v1.ThreadQuery/Index", on_index },
    { "/thread.v1.ThreadLibrary/Library", on_library },
    { "/thread.v1.ThreadLibrary/Documents", on_documents },
};

struct fake {
    int listener;
    int calls;
    size_t route_count;
};

/* A fake threadd answering exactly `calls` connections. */
static void *serve(void *arg) {
    struct fake *f = arg;
    for (int i = 0; i < f->calls; i++) {
        int fd = accept(f->listener, NULL, NULL);
        if (fd < 0) break;
        conduit_serve(fd, routes, f->route_count, NULL);
        close(fd);
    }
    return NULL;
}

static void start(struct fake *f, pthread_t *thread, int calls, size_t route_count) {
    snprintf(dir, sizeof dir, "/tmp/thread-client-XXXXXX");
    MARY_ASSERT(mkdtemp(dir) != NULL);
    snprintf(sock, sizeof sock, "%s/thread.sock", dir);
    f->listener = mc_listen_unix(sock, 0600);
    MARY_ASSERT(f->listener >= 0);
    f->calls = calls;
    f->route_count = route_count;
    pthread_create(thread, NULL, serve, f);
}

static void stop(struct fake *f, pthread_t thread) {
    pthread_join(thread, NULL);
    close(f->listener);
    unlink(sock);
    rmdir(dir);
}

MARY_TEST(a_turns_document_id_carries_its_start) {
    char id[64];
    thread_turn_document_id(1757700000000LL, id, sizeof id);
    MARY_ASSERT_EQ(strncmp(id, "mary-turn-1757700000000-", 24), 0);
    MARY_ASSERT_EQ(strlen(id), 28);
    MARY_ASSERT_EQ(strspn(id + 24, "0123456789abcdef"), 4);
}

MARY_TEST(library_and_documents_reach_threadd) {
    struct fake f;
    pthread_t thread;
    start(&f, &thread, 2, 3);
    Thread__V1__ThreadLibraryResponse *lib = NULL;
    MARY_ASSERT_EQ(thread_client_library(sock, 0, NULL, 2000, &lib, NULL), 0);
    MARY_ASSERT(lib && lib->n_groups == 1);
    if (lib) {
        MARY_ASSERT_STR(lib->groups[0]->id, "memory-rao");
        thread__v1__thread_library_response__free_unpacked(lib, NULL);
    }

    char id[64];
    thread_turn_document_id(1757700000000LL, id, sizeof id);
    const char *ids[] = { id };
    Thread__V1__ThreadDocumentsResponse *docs = NULL;
    MARY_ASSERT_EQ(thread_client_documents(sock, ids, 1, 2000, &docs, NULL), 0);
    MARY_ASSERT(docs && docs->n_documents == 1);
    if (docs) {
        MARY_ASSERT_STR(docs->documents[0]->id, id);
        MARY_ASSERT_STR(docs->documents[0]->texts[1], "Paris.");
        thread__v1__thread_documents_response__free_unpacked(docs, NULL);
    }
    stop(&f, thread);
}

MARY_TEST(a_refusal_or_a_missing_threadd_is_an_error) {
    struct fake f;
    pthread_t thread;
    start(&f, &thread, 1, 1);                   /* serves Index only */
    Thread__V1__ThreadLibraryResponse *lib = NULL;
    int status = 0;
    MARY_ASSERT_EQ(thread_client_library(sock, 0, NULL, 2000, &lib, &status), -EPROTO);
    MARY_ASSERT_EQ(status, CONDUIT_UNIMPLEMENTED);
    MARY_ASSERT(lib == NULL);
    stop(&f, thread);

    Thread__V1__ThreadLibraryResponse *none = NULL;
    int rc = thread_client_library("/tmp/no-such-dir/thread.sock", 0, NULL, 500, &none, NULL);
    MARY_ASSERT(rc == -ENOENT || rc == -ECONNREFUSED);
    MARY_ASSERT(none == NULL);
}

int main(void) {
    mc_ignore_sigpipe();
    MARY_RUN(a_turns_document_id_carries_its_start);
    MARY_RUN(library_and_documents_reach_threadd);
    MARY_RUN(a_refusal_or_a_missing_threadd_is_an_error);
    if (indexed) thread__v1__thread_index_request__free_unpacked(indexed, NULL);
    MARY_TEST_MAIN_END();
}
