#include "thread/service.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/json.h"
#include "common/log.h"
#include "thread/proto.h"

static void pack_reply(conduit_reply *reply, const ProtobufCMessage *message) {
    reply->body_len = protobuf_c_message_get_packed_size(message);
    reply->body = malloc(reply->body_len ? reply->body_len : 1);
    if (!reply->body) {
        reply->status = CONDUIT_RESOURCE_EXHAUSTED;
        snprintf(reply->message, sizeof reply->message, "out of memory");
        return;
    }
    protobuf_c_message_pack(message, reply->body);
}

static void malformed(conduit_reply *reply, const char *what) {
    reply->status = CONDUIT_INVALID_ARGUMENT;
    snprintf(reply->message, sizeof reply->message, "the request is not a %s", what);
}

static void failed(conduit_reply *reply, int rc, const char *owner, const char *what) {
    if (rc == -EINVAL) {
        reply->status = CONDUIT_INVALID_ARGUMENT;
        snprintf(reply->message, sizeof reply->message, "a document or group id is not valid");
    } else if (rc == -EPERM) {
        reply->status = CONDUIT_PERMISSION_DENIED;
        snprintf(reply->message, sizeof reply->message, "a document or group belongs to someone else");
    } else if (rc == -ENOSYS) {
        reply->status = CONDUIT_FAILED_PRECONDITION;
        snprintf(reply->message, sizeof reply->message, "no embedder: sewnd is not reachable");
    } else if (rc == -ENOENT) {
        reply->status = CONDUIT_NOT_FOUND;
        snprintf(reply->message, sizeof reply->message, "no such document or group");
    } else {
        reply->status = CONDUIT_INTERNAL;
        snprintf(reply->message, sizeof reply->message, "the store could not %s", what);
        mc_log(MC_LOG_ERROR, "%s for %s failed: %d", what, owner, rc);
    }
}

static void handle_index(const uint8_t *bytes, size_t len, conduit_reply *reply, void *user) {
    thread_caller *caller = user;
    Thread__V1__ThreadIndexRequest *req = thread__v1__thread_index_request__unpack(NULL, len, bytes);
    if (!req) return malformed(reply, "ThreadIndexRequest");
    const char *owner = thread_caller_owner(caller, req->owner_id);
    size_t indexed = 0;
    int rc = thread_store_index(caller->store, owner, req, &indexed);
    if (rc < 0 && indexed == 0) failed(reply, rc, owner, "index");
    else {
        Thread__V1__ThreadIndexResponse resp = THREAD__V1__THREAD_INDEX_RESPONSE__INIT;
        resp.success = 1;
        resp.indexed_count = (int32_t)indexed;
        resp.full_cids = indexed ? calloc(indexed, sizeof *resp.full_cids) : NULL;
        for (size_t i = 0; i < req->n_items && resp.n_full_cids < indexed; i++)
            if (req->items[i]->n_texts) resp.full_cids[resp.n_full_cids++] = req->items[i]->document_id;
        pack_reply(reply, &resp.base);
        free(resp.full_cids);
        mc_log(MC_LOG_INFO, "indexed %zu document(s) for %s", indexed, owner);
    }
    thread__v1__thread_index_request__free_unpacked(req, NULL);
}

static void handle_search(const uint8_t *bytes, size_t len, conduit_reply *reply, void *user) {
    thread_caller *caller = user;
    Thread__V1__ThreadSearchRequest *req = thread__v1__thread_search_request__unpack(NULL, len, bytes);
    if (!req) return malformed(reply, "ThreadSearchRequest");
    const char *owner = thread_caller_owner(caller, req->owner_id);
    int error = 0;
    Thread__V1__ThreadSearchResponse *resp = thread_store_search_proto(caller->store, owner, req, &error);
    if (!resp) failed(reply, error, owner, "search");
    else {
        pack_reply(reply, &resp->base);
        thread_search_response_free(resp);
    }
    thread__v1__thread_search_request__free_unpacked(req, NULL);
}

static void handle_remove(const uint8_t *bytes, size_t len, conduit_reply *reply, void *user) {
    thread_caller *caller = user;
    Thread__V1__ThreadRemoveRequest *req = thread__v1__thread_remove_request__unpack(NULL, len, bytes);
    if (!req) return malformed(reply, "ThreadRemoveRequest");
    const char *owner = thread_caller_owner(caller, req->owner_id);
    size_t removed = 0;
    int rc = thread_store_remove(caller->store, owner, (const char *const *)req->document_ids, req->n_document_ids, "grpc", &removed);
    if (rc) failed(reply, rc, owner, "remove");
    else {
        Thread__V1__ThreadRemoveResponse resp = THREAD__V1__THREAD_REMOVE_RESPONSE__INIT;
        resp.success = 1;
        resp.removed_count = (int32_t)removed;
        pack_reply(reply, &resp.base);
    }
    thread__v1__thread_remove_request__free_unpacked(req, NULL);
}

static void handle_library(const uint8_t *bytes, size_t len, conduit_reply *reply, void *user) {
    thread_caller *caller = user;
    Thread__V1__ThreadLibraryRequest *req = thread__v1__thread_library_request__unpack(NULL, len, bytes);
    if (!req) return malformed(reply, "ThreadLibraryRequest");
    Thread__V1__ThreadLibraryResponse *resp = thread_store_library(caller->store, thread_caller_owner(caller, req->owner_id), req);
    pack_reply(reply, &resp->base);
    thread_library_response_free(resp);
    thread__v1__thread_library_request__free_unpacked(req, NULL);
}

static void handle_documents(const uint8_t *bytes, size_t len, conduit_reply *reply, void *user) {
    thread_caller *caller = user;
    Thread__V1__ThreadDocumentsRequest *req = thread__v1__thread_documents_request__unpack(NULL, len, bytes);
    if (!req) return malformed(reply, "ThreadDocumentsRequest");
    Thread__V1__ThreadDocumentsResponse *resp = thread_store_documents(caller->store, thread_caller_owner(caller, req->owner_id), req);
    pack_reply(reply, &resp->base);
    thread_documents_response_free(resp);
    thread__v1__thread_documents_request__free_unpacked(req, NULL);
}

static void handle_export(const uint8_t *bytes, size_t len, conduit_reply *reply, void *user) {
    thread_caller *caller = user;
    Thread__V1__ThreadExportCorpusRequest *req = thread__v1__thread_export_corpus_request__unpack(NULL, len, bytes);
    if (!req) return malformed(reply, "ThreadExportCorpusRequest");
    Thread__V1__ThreadExportCorpusResponse *resp = thread_store_export(caller->store, thread_caller_owner(caller, req->owner_id), req);
    pack_reply(reply, &resp->base);
    thread_export_response_free(resp);
    thread__v1__thread_export_corpus_request__free_unpacked(req, NULL);
}

static void handle_graph(const uint8_t *bytes, size_t len, conduit_reply *reply, void *user) {
    thread_caller *caller = user;
    Thread__V1__ThreadGraphQueryRequest *req = thread__v1__thread_graph_query_request__unpack(NULL, len, bytes);
    if (!req) return malformed(reply, "ThreadGraphQueryRequest");
    Thread__V1__ThreadGraphQueryResponse *resp = thread_store_graph_proto(caller->store, thread_caller_owner(caller, req->owner_id), req);
    pack_reply(reply, &resp->base);
    thread_graph_response_free(resp);
    thread__v1__thread_graph_query_request__free_unpacked(req, NULL);
}

static void handle_update_group(const uint8_t *bytes, size_t len, conduit_reply *reply, void *user) {
    thread_caller *caller = user;
    Thread__V1__ThreadUpdateGroupRequest *req = thread__v1__thread_update_group_request__unpack(NULL, len, bytes);
    if (!req) return malformed(reply, "ThreadUpdateGroupRequest");
    bool updated = false;
    int rc = thread_store_update_group(caller->store, thread_caller_owner(caller, req->owner_id), req->group_id, req->access, req->label,
                                       req->group_description, (const char *const *)req->tags, req->n_tags, req->update_metadata, &updated);
    Thread__V1__ThreadUpdateGroupResponse resp = THREAD__V1__THREAD_UPDATE_GROUP_RESPONSE__INIT;
    resp.success = rc == 0 && updated;
    resp.group_id = req->group_id;
    if (rc == -EINVAL) failed(reply, rc, caller->owner, "update");
    else pack_reply(reply, &resp.base);
    thread__v1__thread_update_group_request__free_unpacked(req, NULL);
}

static void handle_update_document(const uint8_t *bytes, size_t len, conduit_reply *reply, void *user) {
    thread_caller *caller = user;
    Thread__V1__ThreadUpdateDocumentRequest *req = thread__v1__thread_update_document_request__unpack(NULL, len, bytes);
    if (!req) return malformed(reply, "ThreadUpdateDocumentRequest");
    bool updated = false;
    int rc = thread_store_update_document(caller->store, thread_caller_owner(caller, req->owner_id), req->document_id, req->access, req->group_id, &updated);
    Thread__V1__ThreadUpdateDocumentResponse resp = THREAD__V1__THREAD_UPDATE_DOCUMENT_RESPONSE__INIT;
    resp.success = rc == 0 && updated;
    resp.document_id = req->document_id;
    if (rc == -EINVAL) failed(reply, rc, caller->owner, "update");
    else pack_reply(reply, &resp.base);
    thread__v1__thread_update_document_request__free_unpacked(req, NULL);
}

static void handle_stats(const uint8_t *bytes, size_t len, conduit_reply *reply, void *user) {
    thread_caller *caller = user;
    struct json_object *stats = thread_store_stats_json(caller->store);
    Thread__V1__ThreadStatsResponse resp = THREAD__V1__THREAD_STATS_RESPONSE__INIT;
    int64_t v = 0;
    mc_json_int64(stats, "documents", &v);
    resp.document_count = v;
    v = 0;
    mc_json_int64(stats, "groups", &v);
    resp.group_count = v;
    v = 0;
    mc_json_int64(stats, "owners", &v);
    resp.owner_count = v;
    resp.available_document_count = 0;
    json_object_put(stats);
    pack_reply(reply, &resp.base);
}

const conduit_route thread_routes[] = {
    { THREAD_PATH_INDEX, handle_index },
    { THREAD_PATH_SEARCH, handle_search },
    { THREAD_PATH_REMOVE, handle_remove },
    { THREAD_PATH_LIBRARY, handle_library },
    { THREAD_PATH_DOCUMENTS, handle_documents },
    { THREAD_PATH_EXPORT, handle_export },
    { THREAD_PATH_GRAPH, handle_graph },
    { THREAD_PATH_UPDATE_GROUP, handle_update_group },
    { THREAD_PATH_UPDATE_DOCUMENT, handle_update_document },
    { THREAD_PATH_STATS, handle_stats },
};
const size_t thread_route_count = sizeof thread_routes / sizeof thread_routes[0];
