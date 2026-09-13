#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "sewn/retrieve.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "common/io.h"
#include "common/json.h"
#include "common/lines.h"

void sewn_retrieval_free(sewn_retrieval *r) {
    for (size_t i = 0; i < r->n; i++) {
        sewn_partition *p = &r->partitions[i];
        free(p->partition_id);
        free(p->document_id);
        free(p->owner_id);
        free(p->text);
        free(p->name);
        free(p->family);
        free(p->lane);
        free(p->group_id);
    }
    free(r->partitions);
    if (r->trace) json_object_put(r->trace);
    memset(r, 0, sizeof *r);
}

static void take_list(struct json_object *arr, const char **out, size_t *n) {
    for (size_t i = 0; arr && i < json_object_array_length(arr) && *n < SEWN_SCOPE_LIST_MAX; i++) {
        const char *s = json_object_get_string(json_object_array_get_idx(arr, i));
        if (s && *s) out[(*n)++] = s;
    }
}

void sewn_scope_parse(struct json_object *request, sewn_scope *out) {
    memset(out, 0, sizeof *out);
    struct json_object *sewn = mc_json_object(request, "sewn");
    out->owner_id = mc_json_string(sewn, "owner_id");
    out->request_id = mc_json_string(sewn, "request_id");
    take_list(mc_json_array(sewn, "lanes"), out->lanes, &out->n_lanes);
    take_list(mc_json_array(sewn, "groups"), out->groups, &out->n_groups);
    const char *group = mc_json_string(mc_json_object(sewn, "group"), "id");
    if (group && *group && out->n_groups < SEWN_SCOPE_LIST_MAX) out->groups[out->n_groups++] = group;
    take_list(mc_json_array(sewn, "entities"), out->entities, &out->n_entities);
    if (!out->n_entities) take_list(mc_json_array(sewn, "tags"), out->entities, &out->n_entities);
    mc_json_bool(sewn, "aggregate", &out->aggregate);
    const char *scope = mc_json_string(sewn, "scope");
    out->global = scope && strcmp(scope, "global") == 0;
}

static struct json_object *strings(const char *const *v, size_t n) {
    struct json_object *arr = json_object_new_array();
    for (size_t i = 0; i < n; i++) json_object_array_add(arr, json_object_new_string(v[i]));
    return arr;
}

struct json_object *sewn_scope_json(const sewn_scope *s) {
    struct json_object *o = json_object_new_object();
    if (s->owner_id) json_object_object_add(o, "owner_id", json_object_new_string(s->owner_id));
    json_object_object_add(o, "lanes", strings(s->lanes, s->n_lanes));
    json_object_object_add(o, "groups", strings(s->groups, s->n_groups));
    json_object_object_add(o, "entities", strings(s->entities, s->n_entities));
    if (s->request_id) json_object_object_add(o, "request_id", json_object_new_string(s->request_id));
    json_object_object_add(o, "aggregate", json_object_new_boolean(s->aggregate));
    json_object_object_add(o, "scope", json_object_new_string(s->global ? "global" : "personal"));
    return o;
}

static char *dup_or_empty(const char *s) { return strdup(s ? s : ""); }

int sewn_retrieval_parse(struct json_object *result, sewn_retrieval *out) {
    memset(out, 0, sizeof *out);
    struct json_object *results = mc_json_array(result, "results");
    size_t n = results ? json_object_array_length(results) : 0;
    out->partitions = n ? calloc(n, sizeof *out->partitions) : NULL;
    if (n && !out->partitions) return -ENOMEM;
    for (size_t i = 0; i < n; i++) {
        struct json_object *r = json_object_array_get_idx(results, i);
        const char *pid = mc_json_string(r, "partition_id");
        /* Sewn keeps the first of duplicate partition ids. */
        bool seen = false;
        for (size_t k = 0; k < out->n && pid; k++) if (strcmp(out->partitions[k].partition_id, pid) == 0) seen = true;
        if (seen) continue;
        sewn_partition *p = &out->partitions[out->n++];
        p->partition_id = dup_or_empty(pid);
        p->document_id = dup_or_empty(mc_json_string(r, "document_id"));
        p->owner_id = dup_or_empty(mc_json_string(r, "owner_id"));
        for (char *c = p->owner_id; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
        p->text = dup_or_empty(mc_json_string(r, "text"));
        const char *name = mc_json_string(r, "name");
        p->name = dup_or_empty(name && *name ? name : mc_json_string(r, "document_id"));
        p->family = dup_or_empty(mc_json_string(r, "family"));
        p->lane = dup_or_empty(mc_json_string(r, "lane"));
        p->group_id = dup_or_empty(mc_json_string(r, "group_id"));
        double score = 0;
        mc_json_double(r, "score", &score);
        p->score = (float)score;
    }
    struct json_object *trace = mc_json_object(result, "trace");
    if (trace) out->trace = json_object_get(trace);
    int64_t ms = 0;
    mc_json_int64(result, "ms", &ms);
    out->ms = ms;
    return 0;
}

/* MARK: - threadd's local socket */

static struct json_object *reply_line;
static int take_line(const char *line, size_t len, void *user) {
    reply_line = mc_json_parse(line, len);
    return 1;
}

/* One request line, one reply line. */
static int local_call(const char *path, struct json_object *request, struct json_object **reply, char *message, size_t cap) {
    *reply = NULL;
    const char *socket_path = path ? path : SEWN_THREAD_LOCAL_SOCKET;
    const char *env = getenv("THREAD_LOCAL_SOCKET");
    if (!path && env && *env) socket_path = env;
    int fd = mc_connect_unix(socket_path);
    if (fd < 0) {
        snprintf(message, cap, "threadd is not reachable at %s: %s", socket_path, strerror(-fd));
        return fd;
    }
    struct timeval patience = { .tv_sec = SEWN_RETRIEVE_TIMEOUT_MS / 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof patience);
    size_t n = 0;
    const char *text = mc_json_compact(request, &n);
    int rc = mc_write_all(fd, text, n);
    if (rc == 0) rc = mc_write_all(fd, "\n", 1);
    if (rc) {
        close(fd);
        snprintf(message, cap, "threadd hung up");
        return rc;
    }
    mc_line_reader reader;
    mc_line_reader_init(&reader, 16u << 20);
    struct json_object *got = NULL;
    char buf[65536];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        reply_line = NULL;
        if (mc_line_reader_feed(&reader, buf, (size_t)r, take_line, NULL) == 1) {
            got = reply_line;
            break;
        }
    }
    mc_line_reader_free(&reader);
    close(fd);
    if (!got) {
        snprintf(message, cap, "threadd did not answer");
        return -ECONNRESET;
    }
    const char *type = mc_json_type(got);
    if (type && strcmp(type, "error") == 0) {
        const char *why = mc_json_string(got, "message");
        snprintf(message, cap, "threadd: %s", why ? why : "error");
        json_object_put(got);
        return -EIO;
    }
    *reply = got;
    return 0;
}

int sewn_thread_retrieve(const sewn_scope *scope, const char *query, int top_k, sewn_retrieval *out, char *message, size_t cap, void *user) {
    memset(out, 0, sizeof *out);
    struct json_object *req = json_object_new_object();
    json_object_object_add(req, "type", json_object_new_string("search"));
    json_object_object_add(req, "source", json_object_new_string("sewnd"));
    if (scope->owner_id && *scope->owner_id) json_object_object_add(req, "owner_id", json_object_new_string(scope->owner_id));
    json_object_object_add(req, "query", json_object_new_string(query));
    json_object_object_add(req, "lanes", strings(scope->lanes, scope->n_lanes));
    json_object_object_add(req, "groups", strings(scope->groups, scope->n_groups));
    json_object_object_add(req, "entities", strings(scope->entities, scope->n_entities));
    json_object_object_add(req, "top_k", json_object_new_int(top_k > 0 ? top_k : SEWN_RETRIEVE_TOP_K));
    if (scope->request_id) json_object_object_add(req, "request_id", json_object_new_string(scope->request_id));
    struct json_object *reply = NULL;
    int rc = local_call(user, req, &reply, message, cap);
    json_object_put(req);
    if (rc) return rc;
    rc = sewn_retrieval_parse(reply, out);
    json_object_put(reply);
    return rc;
}

int sewn_thread_deposit(const char *owner_id, const char *group_id, const char *label, const char *family,
                        const char *const *texts, size_t n, char *document_id, size_t cap, void *user) {
    struct json_object *req = json_object_new_object();
    json_object_object_add(req, "type", json_object_new_string("deposit"));
    json_object_object_add(req, "source", json_object_new_string("sewnd"));
    if (owner_id && *owner_id) json_object_object_add(req, "owner_id", json_object_new_string(owner_id));
    json_object_object_add(req, "group", json_object_new_string(group_id));
    if (label) json_object_object_add(req, "label", json_object_new_string(label));
    if (family) json_object_object_add(req, "family", json_object_new_string(family));
    json_object_object_add(req, "texts", strings(texts, n));
    char message[256] = "";
    struct json_object *reply = NULL;
    int rc = local_call(user, req, &reply, message, sizeof message);
    json_object_put(req);
    if (rc) return rc;
    const char *id = mc_json_string(reply, "document_id");
    if (document_id && cap) snprintf(document_id, cap, "%s", id ? id : "");
    json_object_put(reply);
    return 0;
}
