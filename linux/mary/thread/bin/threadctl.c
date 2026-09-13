/* threadctl: threadd from a terminal, as the user running it.
 *
 *   threadctl library [--limit N] [--after GROUP-ID]
 *   threadctl documents DOCUMENT-ID...
 *   threadctl index --group GROUP-ID [--label TEXT] --document DOCUMENT-ID TEXT... */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/io.h"
#include "conduit/grpc.h"
#include "thread/service.h"

static const char *socket_path(void) {
    const char *path = getenv("THREAD_SOCKET");
    return path && *path ? path : THREAD_SOCKET_PATH;
}

/* Packs `request`, calls `method`, returns the response bytes (the caller frees). */
static uint8_t *call(const char *path, const char *method, const ProtobufCMessage *request, size_t *len) {
    size_t n = protobuf_c_message_get_packed_size(request);
    uint8_t *packed = malloc(n ? n : 1);
    protobuf_c_message_pack(request, packed);
    int fd = mc_connect_unix(path);
    if (fd < 0) {
        fprintf(stderr, "threadctl: cannot reach threadd at %s: %s\n", path, strerror(-fd));
        free(packed);
        return NULL;
    }
    conduit_result result;
    int rc = conduit_call(fd, method, packed, n, 10000, &result);
    close(fd);
    free(packed);
    if (rc < 0) {
        fprintf(stderr, "threadctl: %s\n", strerror(-rc));
        return NULL;
    }
    if (result.status != CONDUIT_OK) {
        fprintf(stderr, "threadctl: threadd answered %d: %s\n", result.status, result.message);
        free(result.body);
        return NULL;
    }
    *len = result.body_len;
    return result.body;
}

static void when(int64_t seconds, char *out, size_t cap) {
    time_t t = (time_t)seconds;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, cap, "%Y-%m-%d %H:%M", &tm);
}

static int library(const char *path, int argc, char **argv) {
    Thread__V1__ThreadLibraryRequest req = THREAD__V1__THREAD_LIBRARY_REQUEST__INIT;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) req.limit = atoi(argv[++i]);
        else if (strcmp(argv[i], "--after") == 0 && i + 1 < argc) req.after_id = argv[++i];
        else return 2;
    }
    size_t len = 0;
    uint8_t *bytes = call(path, THREAD_PATH_LIBRARY, &req.base, &len);
    if (!bytes) return 1;
    Thread__V1__ThreadLibraryResponse *resp = thread__v1__thread_library_response__unpack(NULL, len, bytes);
    free(bytes);
    if (!resp) return 1;
    if (!resp->n_groups) printf("(no groups)\n");
    for (size_t g = 0; g < resp->n_groups; g++) {
        Thread__V1__ThreadGroup *group = resp->groups[g];
        printf("%s  %s  (%zu document%s)\n", group->id, *group->label ? group->label : "-", group->n_documents, group->n_documents == 1 ? "" : "s");
        for (size_t d = 0; d < group->n_documents; d++) {
            char at[32];
            when(group->documents[d]->created_at, at, sizeof at);
            printf("    %s  %s  %s\n", group->documents[d]->id, at, group->documents[d]->name);
        }
    }
    if (resp->has_more) printf("(more after %s)\n", resp->groups[resp->n_groups - 1]->id);
    thread__v1__thread_library_response__free_unpacked(resp, NULL);
    return 0;
}

static int documents(const char *path, int argc, char **argv) {
    if (argc == 0) return 2;
    Thread__V1__ThreadDocumentsRequest req = THREAD__V1__THREAD_DOCUMENTS_REQUEST__INIT;
    req.n_document_ids = (size_t)argc;
    req.document_ids = argv;
    size_t len = 0;
    uint8_t *bytes = call(path, THREAD_PATH_DOCUMENTS, &req.base, &len);
    if (!bytes) return 1;
    Thread__V1__ThreadDocumentsResponse *resp = thread__v1__thread_documents_response__unpack(NULL, len, bytes);
    free(bytes);
    if (!resp) return 1;
    if (!resp->n_documents) printf("(none of those documents are yours)\n");
    for (size_t i = 0; i < resp->n_documents; i++) {
        Thread__V1__ThreadDocumentContent *c = resp->documents[i];
        char at[32];
        when(c->created_at, at, sizeof at);
        printf("== %s  (%s, %s)\n", c->id, *c->group_label ? c->group_label : c->group_id, at);
        for (size_t t = 0; t < c->n_texts; t++) printf("%s\n", c->texts[t]);
    }
    thread__v1__thread_documents_response__free_unpacked(resp, NULL);
    return 0;
}

static int index_document(const char *path, int argc, char **argv) {
    Thread__V1__ThreadIndexRequest req = THREAD__V1__THREAD_INDEX_REQUEST__INIT;
    Thread__V1__ThreadIndexItem item = THREAD__V1__THREAD_INDEX_ITEM__INIT;
    int i = 0;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--group") == 0 && i + 1 < argc) req.group_id = argv[++i];
        else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) req.group_label = argv[++i];
        else if (strcmp(argv[i], "--document") == 0 && i + 1 < argc) item.document_id = argv[++i];
        else break;
    }
    if (!*item.document_id || i == argc) return 2;
    item.n_texts = (size_t)(argc - i);
    item.texts = argv + i;
    Thread__V1__ThreadIndexItem *items[] = { &item };
    req.n_items = 1;
    req.items = items;
    size_t len = 0;
    uint8_t *bytes = call(path, THREAD_PATH_INDEX, &req.base, &len);
    if (!bytes) return 1;
    Thread__V1__ThreadIndexResponse *resp = thread__v1__thread_index_response__unpack(NULL, len, bytes);
    free(bytes);
    if (!resp) return 1;
    printf("indexed %d:", resp->indexed_count);
    for (size_t c = 0; c < resp->n_full_cids; c++) printf(" %s", resp->full_cids[c]);
    printf("\n");
    thread__v1__thread_index_response__free_unpacked(resp, NULL);
    return 0;
}

int main(int argc, char **argv) {
    const char *path = socket_path();
    int i = 1;
    if (i + 1 < argc && strcmp(argv[i], "--socket") == 0) {
        path = argv[i + 1];
        i += 2;
    }
    int rc = 2;
    if (i < argc && strcmp(argv[i], "library") == 0) rc = library(path, argc - i - 1, argv + i + 1);
    else if (i < argc && strcmp(argv[i], "documents") == 0) rc = documents(path, argc - i - 1, argv + i + 1);
    else if (i < argc && strcmp(argv[i], "index") == 0) rc = index_document(path, argc - i - 1, argv + i + 1);
    if (rc == 2)
        fprintf(stderr, "usage: threadctl [--socket PATH] library [--limit N] [--after GROUP-ID]\n"
                        "       threadctl [--socket PATH] documents DOCUMENT-ID...\n"
                        "       threadctl [--socket PATH] index --group GROUP-ID [--label TEXT] --document DOCUMENT-ID TEXT...\n");
    return rc;
}
