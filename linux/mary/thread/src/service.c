#include "thread/service.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/log.h"

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

static void handle_index(const uint8_t *bytes, size_t len, conduit_reply *reply, void *user) {
    thread_caller *caller = user;
    Thread__V1__ThreadIndexRequest *req = thread__v1__thread_index_request__unpack(NULL, len, bytes);
    if (!req) return malformed(reply, "ThreadIndexRequest");
    size_t indexed = 0;
    int rc = thread_store_index(caller->store, caller->owner, req, &indexed);
    if (rc == -EINVAL) {
        reply->status = CONDUIT_INVALID_ARGUMENT;
        snprintf(reply->message, sizeof reply->message, "a document or group id is not valid");
    } else if (rc == -EPERM) {
        reply->status = CONDUIT_PERMISSION_DENIED;
        snprintf(reply->message, sizeof reply->message, "a document or group belongs to someone else");
    } else if (rc < 0) {
        reply->status = CONDUIT_INTERNAL;
        snprintf(reply->message, sizeof reply->message, "the store could not be written");
        mc_log(MC_LOG_ERROR, "index for %s failed: %d", caller->owner, rc);
    } else {
        Thread__V1__ThreadIndexResponse resp = THREAD__V1__THREAD_INDEX_RESPONSE__INIT;
        resp.success = 1;
        resp.indexed_count = (int32_t)indexed;
        resp.full_cids = indexed ? calloc(indexed, sizeof *resp.full_cids) : NULL;
        for (size_t i = 0; i < req->n_items && resp.n_full_cids < indexed; i++)
            if (req->items[i]->n_texts) resp.full_cids[resp.n_full_cids++] = req->items[i]->document_id;
        pack_reply(reply, &resp.base);
        free(resp.full_cids);
        mc_log(MC_LOG_INFO, "indexed %zu document(s) for %s", indexed, caller->owner);
    }
    thread__v1__thread_index_request__free_unpacked(req, NULL);
}

static void handle_library(const uint8_t *bytes, size_t len, conduit_reply *reply, void *user) {
    thread_caller *caller = user;
    Thread__V1__ThreadLibraryRequest *req = thread__v1__thread_library_request__unpack(NULL, len, bytes);
    if (!req) return malformed(reply, "ThreadLibraryRequest");
    Thread__V1__ThreadLibraryResponse *resp = thread_store_library(caller->store, caller->owner, req);
    pack_reply(reply, &resp->base);
    thread_library_response_free(resp);
    thread__v1__thread_library_request__free_unpacked(req, NULL);
}

static void handle_documents(const uint8_t *bytes, size_t len, conduit_reply *reply, void *user) {
    thread_caller *caller = user;
    Thread__V1__ThreadDocumentsRequest *req = thread__v1__thread_documents_request__unpack(NULL, len, bytes);
    if (!req) return malformed(reply, "ThreadDocumentsRequest");
    Thread__V1__ThreadDocumentsResponse *resp = thread_store_documents(caller->store, caller->owner, req);
    pack_reply(reply, &resp->base);
    thread_documents_response_free(resp);
    thread__v1__thread_documents_request__free_unpacked(req, NULL);
}

const conduit_route thread_routes[] = {
    { THREAD_PATH_INDEX, handle_index },
    { THREAD_PATH_LIBRARY, handle_library },
    { THREAD_PATH_DOCUMENTS, handle_documents },
};
const size_t thread_route_count = sizeof thread_routes / sizeof thread_routes[0];
