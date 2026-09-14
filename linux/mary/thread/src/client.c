#include "thread/client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/io.h"
#include "common/json.h"
#include "conduit/grpc.h"

#define PATH_LIBRARY "/thread.v1.ThreadLibrary/Library"
#define PATH_DOCUMENTS "/thread.v1.ThreadLibrary/Documents"

const char *thread_client_default_socket(void) {
    const char *path = getenv("THREAD_SOCKET");
    return path && *path ? path : THREAD_CLIENT_SOCKET_PATH;
}

void thread_turn_document_id(int64_t started_ms, char *out, size_t cap) {
    unsigned char r[2] = { 0, 0 };
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, r, sizeof r) != (ssize_t)sizeof r) {
        unsigned v = (unsigned)rand();
        r[0] = (unsigned char)v;
        r[1] = (unsigned char)(v >> 8);
    }
    if (fd >= 0) close(fd);
    snprintf(out, cap, "mary-turn-%lld-%02x%02x", (long long)started_ms, r[0], r[1]);
}

static int call(const char *path, const char *method, const uint8_t *request, size_t len, int timeout_ms,
                conduit_result *res, int *status) {
    int fd = conduit_connect_unix(path ? path : thread_client_default_socket());
    if (fd < 0) return fd;
    int rc = conduit_call(fd, method, request, len, timeout_ms > 0 ? timeout_ms : THREAD_CLIENT_TIMEOUT_MS, res);
    close(fd);
    if (rc < 0) return rc;
    if (status) *status = res->status;
    if (res->status != CONDUIT_OK) {
        free(res->body);
        res->body = NULL;
        return -EPROTO;
    }
    return 0;
}

int thread_client_library(const char *socket_path, int limit, const char *after_id, int timeout_ms,
               Thread__V1__ThreadLibraryResponse **out, int *status) {
    Thread__V1__ThreadLibraryRequest req = THREAD__V1__THREAD_LIBRARY_REQUEST__INIT;
    req.limit = limit;
    if (after_id) req.after_id = (char *)after_id;
    size_t n = thread__v1__thread_library_request__get_packed_size(&req);
    uint8_t *packed = malloc(n ? n : 1);
    if (!packed) return -ENOMEM;
    thread__v1__thread_library_request__pack(&req, packed);
    conduit_result res;
    int rc = call(socket_path, PATH_LIBRARY, packed, n, timeout_ms, &res, status);
    free(packed);
    if (rc < 0) return rc;
    *out = thread__v1__thread_library_response__unpack(NULL, res.body_len, res.body);
    free(res.body);
    return *out ? 0 : -EPROTO;
}

int thread_client_documents(const char *socket_path, const char *const *ids, size_t count, int timeout_ms,
                 Thread__V1__ThreadDocumentsResponse **out, int *status) {
    Thread__V1__ThreadDocumentsRequest req = THREAD__V1__THREAD_DOCUMENTS_REQUEST__INIT;
    req.n_document_ids = count;
    req.document_ids = (char **)ids;
    size_t n = thread__v1__thread_documents_request__get_packed_size(&req);
    uint8_t *packed = malloc(n ? n : 1);
    if (!packed) return -ENOMEM;
    thread__v1__thread_documents_request__pack(&req, packed);
    conduit_result res;
    int rc = call(socket_path, PATH_DOCUMENTS, packed, n, timeout_ms, &res, status);
    free(packed);
    if (rc < 0) return rc;
    *out = thread__v1__thread_documents_response__unpack(NULL, res.body_len, res.body);
    free(res.body);
    return *out ? 0 : -EPROTO;
}
