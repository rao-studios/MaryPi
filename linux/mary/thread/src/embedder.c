#include "thread/embedder.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/base64.h"
#include "common/json.h"
#include "common/service.h"
#include "thread/db.h"

struct sewn_user {
    char path[256];
};

static int error_of(struct json_object *reply, char *message, size_t cap) {
    const char *msg = mc_json_string(reply, "message");
    int64_t status = 0;
    mc_json_int64(reply, "status", &status);
    snprintf(message, cap, "%s", msg ? msg : "sewnd answered with an error");
    const char *stage = mc_json_string(reply, "stage");
    if (status == 429) return -EAGAIN;
    if (stage && strcmp(stage, "key") == 0) return -ENOENT;
    return -EIO;
}

static int sewn_embed(const char *const *texts, size_t n, float *out, size_t dim, char *message, size_t cap, void *user) {
    struct sewn_user *u = user;
    int fd = mc_service_connect(u->path);
    if (fd < 0) {
        snprintf(message, cap, "sewnd is not reachable at %s: %s", u->path, strerror(-fd));
        return fd == -ENOENT ? -EIO : fd;
    }
    struct json_object *req = json_object_new_object(), *arr = json_object_new_array();
    json_object_object_add(req, "type", json_object_new_string("embed"));
    json_object_object_add(req, "purpose", json_object_new_string("index"));
    for (size_t i = 0; i < n; i++) json_object_array_add(arr, json_object_new_string(texts[i]));
    json_object_object_add(req, "texts", arr);
    struct json_object *reply = NULL;
    int rc = mc_service_call(fd, req, &reply);
    json_object_put(req);
    close(fd);
    if (rc) {
        snprintf(message, cap, "sewnd: %s", strerror(-rc));
        return -EIO;
    }
    const char *type = mc_json_type(reply);
    if (!type || strcmp(type, "embed.result") != 0) {
        rc = error_of(reply, message, cap);
        json_object_put(reply);
        return rc;
    }
    struct json_object *vectors = mc_json_array(reply, "vectors_b64");
    int64_t got_dim = 0;
    mc_json_int64(reply, "dim", &got_dim);
    if (!vectors || json_object_array_length(vectors) != n || (got_dim && (size_t)got_dim != dim)) {
        snprintf(message, cap, "sewnd answered %zu vectors of %lld dims, wanted %zu of %zu",
                 vectors ? json_object_array_length(vectors) : 0, (long long)got_dim, n, dim);
        json_object_put(reply);
        return -EIO;
    }
    for (size_t i = 0; i < n; i++) {
        const char *b64 = json_object_get_string(json_object_array_get_idx(vectors, i));
        size_t len = 0;
        unsigned char *bytes = b64 ? mc_base64_decode_alloc(b64, strlen(b64), &len) : NULL;
        if (!bytes || len != dim * sizeof(float)) {
            free(bytes);
            snprintf(message, cap, "sewnd's vector %zu is not %zu float32s", i, dim);
            json_object_put(reply);
            return -EIO;
        }
        memcpy(out + i * dim, bytes, len);
        free(bytes);
    }
    json_object_put(reply);
    return 0;
}

static int sewn_extract(const char *const *texts, size_t n, const char *prompt, char **json_out, char *message, size_t cap, void *user) {
    struct sewn_user *u = user;
    *json_out = NULL;
    int fd = mc_service_connect(u->path);
    if (fd < 0) {
        snprintf(message, cap, "sewnd is not reachable at %s: %s", u->path, strerror(-fd));
        return -EIO;
    }
    struct json_object *req = json_object_new_object(), *arr = json_object_new_array();
    json_object_object_add(req, "type", json_object_new_string("graph.extract"));
    for (size_t i = 0; i < n; i++) json_object_array_add(arr, json_object_new_string(texts[i]));
    json_object_object_add(req, "texts", arr);
    json_object_object_add(req, "prompt", json_object_new_string(prompt ? prompt : ""));
    struct json_object *reply = NULL;
    int rc = mc_service_call(fd, req, &reply);
    json_object_put(req);
    close(fd);
    if (rc) {
        snprintf(message, cap, "sewnd: %s", strerror(-rc));
        return -EIO;
    }
    const char *type = mc_json_type(reply);
    if (!type || strcmp(type, "graph.extract.result") != 0) {
        rc = error_of(reply, message, cap);
        json_object_put(reply);
        return rc;
    }
    const char *json = mc_json_string(reply, "json");
    *json_out = strdup(json ? json : "");
    json_object_put(reply);
    return *json_out ? 0 : -ENOMEM;
}

int thread_sewn_embedder_init(thread_embedder *e, const char *socket_path) {
    struct sewn_user *u = calloc(1, sizeof *u);
    if (!u) return -ENOMEM;
    snprintf(u->path, sizeof u->path, "%s", socket_path ? socket_path : mc_service_default_socket("SEWN_SOCKET", THREAD_SEWN_SOCKET));
    e->embed = sewn_embed;
    e->extract = sewn_extract;
    e->user = u;
    e->dim = THREAD_EMBEDDING_DIM;
    e->batch_max = THREAD_EMBED_BATCH;
    return 0;
}

void thread_sewn_embedder_free(thread_embedder *e) {
    free(e->user);
    memset(e, 0, sizeof *e);
}

int thread_embedder_embed(const thread_embedder *e, const char *const *texts, size_t n, float *out, char *message, size_t cap) {
    if (!e || !e->embed) {
        snprintf(message, cap, "no embedder");
        return -ENOSYS;
    }
    size_t batch = e->batch_max ? e->batch_max : THREAD_EMBED_BATCH;
    for (size_t at = 0; at < n; at += batch) {
        size_t count = n - at < batch ? n - at : batch;
        int rc = e->embed(texts + at, count, out + at * e->dim, e->dim, message, cap, e->user);
        if (rc) return rc;
    }
    return 0;
}
