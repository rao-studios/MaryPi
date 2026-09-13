#include "mary-thread/client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/io.h"
#include "common/json.h"
#include "conduit/grpc.h"

#define PATH_INDEX "/thread.v1.ThreadQuery/Index"
#define PATH_LIBRARY "/thread.v1.ThreadLibrary/Library"
#define PATH_DOCUMENTS "/thread.v1.ThreadLibrary/Documents"
#define NAME_MAX_BYTES 60

const char *mt_default_socket(void) {
    const char *path = getenv("THREAD_SOCKET");
    return path && *path ? path : MT_SOCKET_PATH;
}

void mt_turn_document_id(int64_t started_ms, char *out, size_t cap) {
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

/* The document's name: the question, cut to NAME_MAX_BYTES on a UTF-8 boundary. */
static void turn_name(const char *text, char *out, size_t cap) {
    size_t n = strlen(text);
    if (n > NAME_MAX_BYTES) {
        n = NAME_MAX_BYTES;
        while (n && ((unsigned char)text[n] & 0xC0) == 0x80) n--;
    }
    if (n >= cap) n = cap - 1;
    memcpy(out, text, n);
    out[n] = 0;
}

uint8_t *mt_turn_index_request(const mt_turn *turn, size_t *len, char *document_id, size_t cap) {
    if (!turn || !turn->user_text || !*turn->user_text) return NULL;
    char id[64], name[NAME_MAX_BYTES + 1];
    mt_turn_document_id(turn->started_ms, id, sizeof id);
    turn_name(turn->user_text, name, sizeof name);

    struct json_object *meta = json_object_new_object();
    json_object_object_add(meta, "source", json_object_new_string(turn->source ? turn->source : "typed"));
    if (turn->model) json_object_object_add(meta, "model", json_object_new_string(turn->model));
    json_object_object_add(meta, "started_ms", json_object_new_int64(turn->started_ms));
    json_object_object_add(meta, "ended_ms", json_object_new_int64(turn->ended_ms));
    json_object_object_add(meta, "cancelled", json_object_new_boolean(turn->cancelled));
    size_t meta_len = 0;
    const char *meta_text = mc_json_compact(meta, &meta_len);

    char *texts[2] = { (char *)turn->user_text, (char *)(turn->reply ? turn->reply : "") };
    Thread__V1__ThreadIndexItem item = THREAD__V1__THREAD_INDEX_ITEM__INIT;
    item.document_id = id;
    item.name = name;
    item.media_type = "text/plain";
    item.n_texts = 2;
    item.texts = texts;
    item.metadata.data = (uint8_t *)meta_text;
    item.metadata.len = meta_len;
    Thread__V1__ThreadIndexItem *items[] = { &item };
    Thread__V1__ThreadIndexRequest req = THREAD__V1__THREAD_INDEX_REQUEST__INIT;
    req.owner_id = (char *)(turn->owner_id ? turn->owner_id : "");
    req.group_id = MT_GROUP_ID;
    req.group_label = MT_GROUP_LABEL;
    req.scope = MT_SCOPE;
    req.n_items = 1;
    req.items = items;

    size_t n = thread__v1__thread_index_request__get_packed_size(&req);
    uint8_t *packed = malloc(n ? n : 1);
    if (packed) {
        thread__v1__thread_index_request__pack(&req, packed);
        *len = n;
        if (document_id && cap) snprintf(document_id, cap, "%s", id);
    }
    json_object_put(meta);
    return packed;
}

/* One unary call on its own connection. On 0, res->body is the caller's to free. */
static int call(const char *path, const char *method, const uint8_t *request, size_t len, int timeout_ms,
                conduit_result *res, int *status) {
    int fd = conduit_connect_unix(path ? path : mt_default_socket());
    if (fd < 0) return fd;
    int rc = conduit_call(fd, method, request, len, timeout_ms > 0 ? timeout_ms : MT_TIMEOUT_MS, res);
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

int mt_deposit_turn(const char *socket_path, const mt_turn *turn, int timeout_ms, char *document_id, size_t cap, int *status) {
    size_t len = 0;
    char id[64];
    uint8_t *request = mt_turn_index_request(turn, &len, id, sizeof id);
    if (!request) return turn && turn->user_text && *turn->user_text ? -ENOMEM : -EINVAL;
    conduit_result res;
    int rc = call(socket_path, PATH_INDEX, request, len, timeout_ms, &res, status);
    free(request);
    if (rc < 0) return rc;
    Thread__V1__ThreadIndexResponse *resp = thread__v1__thread_index_response__unpack(NULL, res.body_len, res.body);
    free(res.body);
    rc = resp && resp->success && resp->indexed_count == 1 ? 0 : -EPROTO;
    if (resp) thread__v1__thread_index_response__free_unpacked(resp, NULL);
    if (rc == 0 && document_id && cap) snprintf(document_id, cap, "%s", id);
    return rc;
}

int mt_library(const char *socket_path, int limit, const char *after_id, int timeout_ms,
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

int mt_documents(const char *socket_path, const char *const *ids, size_t count, int timeout_ms,
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
