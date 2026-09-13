#include "index/client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/json.h"
#include "common/jsonl.h"

const char *ix_client_socket(const ix_client *c) {
    if (c->socket_path && *c->socket_path) return c->socket_path;
    const char *env = getenv("THREAD_LOCAL_SOCKET");
    return env && *env ? env : IX_THREAD_SOCKET;
}

int ix_call(const ix_client *c, struct json_object *request, struct json_object **reply, char *message, size_t cap) {
    int rc = mc_jsonl_call(ix_client_socket(c), IX_CALL_TIMEOUT_MS, request, reply, message, cap);
    if (rc == -EIO && *reply) {
        json_object_put(*reply);
        *reply = NULL;
    }
    return rc;
}

static struct json_object *typed(const char *type) {
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "type", json_object_new_string(type));
    json_object_object_add(o, "source", json_object_new_string("indexd"));
    return o;
}

int ix_deposit(const ix_client *c, struct json_object *record, char *document_id, size_t cap, char *message, size_t msg_cap) {
    struct json_object *reply = NULL;
    int rc = ix_call(c, record, &reply, message, msg_cap);
    if (rc) return rc;
    const char *id = mc_json_string(reply, "document_id");
    if (document_id && cap) snprintf(document_id, cap, "%s", id ? id : "");
    json_object_put(reply);
    return 0;
}

int ix_file_move(const ix_client *c, const char *from, const char *to, char *message, size_t cap) {
    struct json_object *req = typed("file.move"), *reply = NULL;
    json_object_object_add(req, "from", json_object_new_string(from));
    json_object_object_add(req, "to", json_object_new_string(to));
    int rc = ix_call(c, req, &reply, message, cap);
    json_object_put(req);
    if (reply) json_object_put(reply);
    return rc;
}

int ix_file_remove(const ix_client *c, const char *path, bool *removed, char *message, size_t cap) {
    struct json_object *req = typed("file.remove"), *reply = NULL;
    json_object_object_add(req, "path", json_object_new_string(path));
    int rc = ix_call(c, req, &reply, message, cap);
    json_object_put(req);
    if (rc) return rc;
    bool r = false;
    mc_json_bool(reply, "removed", &r);
    if (removed) *removed = r;
    json_object_put(reply);
    return 0;
}

int ix_parity_report(const ix_client *c, int64_t *run_id, struct json_object *entries, bool last, struct json_object **report, char *message, size_t cap) {
    *report = NULL;
    struct json_object *req = typed("parity.report");
    if (*run_id) json_object_object_add(req, "run_id", json_object_new_int64(*run_id));
    json_object_object_add(req, "entries", json_object_get(entries));
    json_object_object_add(req, "last", json_object_new_boolean(last));
    int rc = ix_call(c, req, report, message, cap);
    json_object_put(req);
    if (rc) return rc;
    int64_t run = 0;
    if (mc_json_int64(*report, "run_id", &run)) *run_id = run;
    return 0;
}
