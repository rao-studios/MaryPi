#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <dirent.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mary_test.h"
#include "thread/service.h"
#include "thread/store.h"

/* rm -rf for a test's own temporary directory, without a shell. */
static void remove_tree(const char *path) {
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char child[512];
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            struct stat st;
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(child);
            else unlink(child);
        }
        closedir(dir);
    }
    rmdir(path);
}

static char dir[64];
static thread_store *store;

struct serving {
    int fd;
    const char *owner;
};

static void *serve(void *arg) {
    struct serving *s = arg;
    thread_caller caller = { .store = store };
    snprintf(caller.owner, sizeof caller.owner, "%s", s->owner);
    conduit_serve(s->fd, thread_routes, thread_route_count, &caller);
    close(s->fd);
    return NULL;
}

/* One gRPC call to a threadd connection owned by `owner`. */
static int call(const char *owner, const char *method, const ProtobufCMessage *request, conduit_result *result) {
    int sv[2];
    MARY_ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    struct serving serving = { sv[1], owner };
    pthread_t thread;
    pthread_create(&thread, NULL, serve, &serving);
    size_t n = request ? protobuf_c_message_get_packed_size(request) : 3;
    uint8_t *packed = malloc(n);
    if (request) protobuf_c_message_pack(request, packed);
    else memcpy(packed, "\xff\xff\xff", 3);
    int rc = conduit_call(sv[0], method, packed, n, 5000, result);
    free(packed);
    close(sv[0]);
    pthread_join(thread, NULL);
    return rc;
}

MARY_TEST(threadd_indexes_lists_and_reads_over_grpc) {
    snprintf(dir, sizeof dir, "/tmp/threadd-XXXXXX");
    MARY_ASSERT(mkdtemp(dir) != NULL);
    int error = 0;
    store = thread_store_open(dir, &error);
    MARY_ASSERT(store != NULL);

    char *texts[] = { "What is the capital of France?", "Paris." };
    Thread__V1__ThreadIndexItem item = THREAD__V1__THREAD_INDEX_ITEM__INIT;
    item.document_id = "mary-turn-1757700000000-ab12";
    item.n_texts = 2;
    item.texts = texts;
    item.metadata.data = (uint8_t *)"{\"source\":\"voice\"}";
    item.metadata.len = 18;
    Thread__V1__ThreadIndexItem *items[] = { &item };
    Thread__V1__ThreadIndexRequest index = THREAD__V1__THREAD_INDEX_REQUEST__INIT;
    index.owner_id = "somebody-else";          /* ignored: the connection's owner wins */
    index.group_id = "mary-conversations";
    index.group_label = "Conversations";
    index.scope = "personal";
    index.n_items = 1;
    index.items = items;
    conduit_result r;
    MARY_ASSERT_EQ(call("mary", THREAD_PATH_INDEX, &index.base, &r), 0);
    MARY_ASSERT_EQ(r.status, CONDUIT_OK);
    Thread__V1__ThreadIndexResponse *indexed = thread__v1__thread_index_response__unpack(NULL, r.body_len, r.body);
    MARY_ASSERT(indexed && indexed->success);
    MARY_ASSERT_EQ(indexed->indexed_count, 1);
    MARY_ASSERT_STR(indexed->full_cids[0], "mary-turn-1757700000000-ab12");
    thread__v1__thread_index_response__free_unpacked(indexed, NULL);
    free(r.body);

    Thread__V1__ThreadLibraryRequest lib = THREAD__V1__THREAD_LIBRARY_REQUEST__INIT;
    MARY_ASSERT_EQ(call("mary", THREAD_PATH_LIBRARY, &lib.base, &r), 0);
    Thread__V1__ThreadLibraryResponse *groups = thread__v1__thread_library_response__unpack(NULL, r.body_len, r.body);
    MARY_ASSERT_EQ(groups->n_groups, 1);
    MARY_ASSERT_STR(groups->groups[0]->owner_id, "mary");
    thread__v1__thread_library_response__free_unpacked(groups, NULL);
    free(r.body);

    MARY_ASSERT_EQ(call("guest", THREAD_PATH_LIBRARY, &lib.base, &r), 0);
    groups = thread__v1__thread_library_response__unpack(NULL, r.body_len, r.body);
    MARY_ASSERT_EQ(groups->n_groups, 0);
    thread__v1__thread_library_response__free_unpacked(groups, NULL);
    free(r.body);

    char *ids[] = { "mary-turn-1757700000000-ab12" };
    Thread__V1__ThreadDocumentsRequest docs = THREAD__V1__THREAD_DOCUMENTS_REQUEST__INIT;
    docs.n_document_ids = 1;
    docs.document_ids = ids;
    MARY_ASSERT_EQ(call("mary", THREAD_PATH_DOCUMENTS, &docs.base, &r), 0);
    Thread__V1__ThreadDocumentsResponse *content = thread__v1__thread_documents_response__unpack(NULL, r.body_len, r.body);
    MARY_ASSERT_EQ(content->n_documents, 1);
    MARY_ASSERT_EQ(content->documents[0]->n_texts, 2);
    MARY_ASSERT_STR(content->documents[0]->texts[1], "Paris.");
    MARY_ASSERT_STR(content->documents[0]->group_label, "Conversations");
    thread__v1__thread_documents_response__free_unpacked(content, NULL);
    free(r.body);

    MARY_ASSERT_EQ(call("mary", "/thread.v1.ThreadQuery/Search", &lib.base, &r), 0);
    MARY_ASSERT_EQ(r.status, CONDUIT_UNIMPLEMENTED);
    MARY_ASSERT_EQ(call("mary", THREAD_PATH_INDEX, NULL, &r), 0);
    MARY_ASSERT_EQ(r.status, CONDUIT_INVALID_ARGUMENT);

    index.group_id = "../../escape";
    MARY_ASSERT_EQ(call("mary", THREAD_PATH_INDEX, &index.base, &r), 0);
    MARY_ASSERT_EQ(r.status, CONDUIT_INVALID_ARGUMENT);
    index.group_id = "mary-conversations";
    MARY_ASSERT_EQ(call("guest", THREAD_PATH_INDEX, &index.base, &r), 0);
    MARY_ASSERT_EQ(r.status, CONDUIT_PERMISSION_DENIED);

    thread_store_close(store);
    remove_tree(dir);
}

int main(void) {
    MARY_RUN(threadd_indexes_lists_and_reads_over_grpc);
    MARY_TEST_MAIN_END();
}
