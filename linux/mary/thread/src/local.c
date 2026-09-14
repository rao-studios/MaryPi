#include "thread/local.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/io.h"
#include "common/json.h"
#include "common/lines.h"
#include "common/log.h"
#include "thread/families.h"
#include "thread/ledger.h"

const char *thread_caller_owner(const thread_caller *caller, const char *requested) {
    return caller->trusted && requested && *requested ? requested : caller->owner;
}

static struct json_object *error_reply(int rc, const char *message) {
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "type", json_object_new_string("error"));
    const char *code = rc == -EINVAL ? "EINVAL" : rc == -EPERM ? "EPERM" : rc == -ENOENT ? "ENOENT" : rc == -ENOSYS ? "ENOSYS" : "EIO";
    json_object_object_add(o, "code", json_object_new_string(code));
    json_object_object_add(o, "message", json_object_new_string(message ? message : strerror(-rc)));
    return o;
}

static struct json_object *result(const char *type, struct json_object *body) {
    struct json_object *o = body ? body : json_object_new_object();
    char full[64];
    snprintf(full, sizeof full, "%s.result", type);
    json_object_object_del(o, "type");
    json_object_object_add(o, "type", json_object_new_string(full));
    return o;
}

static const char **strings(struct json_object *req, const char *key, size_t *n) {
    *n = 0;
    struct json_object *arr = mc_json_array(req, key);
    if (!arr || !json_object_array_length(arr)) return NULL;
    const char **v = calloc(json_object_array_length(arr), sizeof *v);
    for (size_t i = 0; v && i < json_object_array_length(arr); i++) v[(*n)++] = json_object_get_string(json_object_array_get_idx(arr, i));
    return v;
}

static int int_of(struct json_object *req, const char *key, int fallback) {
    int64_t v = 0;
    return mc_json_int64(req, key, &v) ? (int)v : fallback;
}

static const char *source_of(struct json_object *req) {
    const char *s = mc_json_string(req, "source");
    return s && *s ? s : "desktop";
}

struct json_object *thread_local_handle(thread_caller *caller, struct json_object *req) {
    const char *type = mc_json_type(req);
    if (!type) return error_reply(-EINVAL, "the request has no type");
    thread_store *s = caller->store;
    const char *owner = thread_caller_owner(caller, mc_json_string(req, "owner_id"));
    if (strcmp(type, "stats") == 0) return result(type, thread_store_stats_json(s));
    if (strcmp(type, "schemas") == 0) {
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "families", thread_store_schemas_json(s));
        return result(type, o);
    }
    if (strcmp(type, "library") == 0) {
        size_t n = 0;
        const char **ids = strings(req, "document_ids", &n);
        struct json_object *o = thread_store_library_json(s, owner, int_of(req, "limit", 0), mc_json_string(req, "after"), ids, n);
        free(ids);
        return result(type, o);
    }
    if (strcmp(type, "documents") == 0) {
        size_t n = 0;
        const char **ids = strings(req, "ids", &n);
        struct json_object *o = thread_store_documents_json(s, owner, ids, n);
        free(ids);
        return result(type, o);
    }
    if (strcmp(type, "export") == 0) {
        size_t n = 0;
        const char **groups = strings(req, "groups", &n);
        struct json_object *o = thread_store_export_json(s, owner, groups, n, mc_json_string(req, "prefix"), mc_json_string(req, "after"), int_of(req, "limit", 0));
        free(groups);
        return result(type, o);
    }
    if (strcmp(type, "search") == 0) {
        size_t n_entities = 0, n_groups = 0, n_lanes = 0;
        const char **entities = strings(req, "entities", &n_entities), **groups = strings(req, "groups", &n_groups), **lanes = strings(req, "lanes", &n_lanes);
        for (size_t i = 0; i < n_lanes; i++) {
            if (!thread_lane_valid(lanes[i])) {
                free(entities);
                free(groups);
                free(lanes);
                return error_reply(-EINVAL, "a lane is not personal or behavioral");
            }
        }
        thread_search_request q = { .query_text = mc_json_string(req, "query"), .entities = entities, .n_entities = n_entities, .group_ids = groups, .n_groups = n_groups,
                                    .lanes = lanes, .n_lanes = n_lanes, .top_k = int_of(req, "top_k", 0), .request_id = mc_json_string(req, "request_id"), .source = source_of(req) };
        struct json_object *o = NULL;
        int rc = thread_store_search(s, owner, &q, &o);
        free(entities);
        free(groups);
        free(lanes);
        return rc ? error_reply(rc, rc == -ENOSYS ? "no embedder: sewnd is not reachable" : "the search failed") : result(type, o);
    }
    if (strcmp(type, "deposit") == 0) {
        if (!mc_json_string(req, "source")) json_object_object_add(req, "source", json_object_new_string("desktop"));
        struct json_object *o = NULL;
        int rc = thread_store_deposit_json(s, owner, req, &o);
        return rc ? error_reply(rc, rc == -EINVAL ? "the deposit has no text, or an id is not valid" : rc == -EPERM ? "that document or group belongs to someone else" : NULL) : result(type, o);
    }
    if (strcmp(type, "remove") == 0) {
        size_t n = 0;
        const char **ids = strings(req, "ids", &n);
        bool all = false;
        mc_json_bool(req, "all", &all);
        if (!n && !all) {
            free(ids);
            return error_reply(-EINVAL, "name ids, or all: true");
        }
        size_t removed = 0;
        int rc = thread_store_remove(s, owner, ids, n, source_of(req), &removed);
        free(ids);
        if (rc) return error_reply(rc, NULL);
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "removed", json_object_new_int64((int64_t)removed));
        return result(type, o);
    }
    if (strcmp(type, "update.group") == 0) {
        size_t n = 0;
        const char **tags = strings(req, "tags", &n);
        bool updated = false;
        int rc = thread_store_update_group(s, owner, mc_json_string(req, "group"), mc_json_string(req, "access"), mc_json_string(req, "label"),
                                           mc_json_string(req, "description"), tags, n, mc_json_array(req, "tags") != NULL || mc_json_string(req, "description") != NULL, &updated);
        free(tags);
        if (rc) return error_reply(rc, NULL);
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "updated", json_object_new_boolean(updated));
        return result(type, o);
    }
    if (strcmp(type, "update.document") == 0) {
        bool updated = false;
        int rc = thread_store_update_document(s, owner, mc_json_string(req, "document_id"), mc_json_string(req, "access"), mc_json_string(req, "group"), &updated);
        if (rc) return error_reply(rc, NULL);
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "updated", json_object_new_boolean(updated));
        return result(type, o);
    }
    if (strcmp(type, "graph") == 0) {
        size_t n = 0;
        const char **kinds = strings(req, "kinds", &n);
        bool documents = false;
        mc_json_bool(req, "documents", &documents);
        thread_graph_request q = { .entity = mc_json_string(req, "entity"), .query = mc_json_string(req, "query"), .kinds = kinds, .n_kinds = n,
                                   .hops = int_of(req, "hops", 1), .limit = int_of(req, "limit", 20), .include_documents = documents };
        struct json_object *o = NULL;
        int rc = thread_store_graph_query(s, owner, &q, &o);
        free(kinds);
        return rc ? error_reply(rc, NULL) : result(type, o);
    }
    if (strcmp(type, "graph.mutate") == 0) {
        struct json_object *o = NULL;
        int rc = thread_store_graph_mutate(s, owner, mc_json_string(req, "op"), mc_json_string(req, "id"), mc_json_string(req, "name"),
                                           mc_json_string(req, "into"), mc_json_string(req, "kind"), source_of(req), &o);
        return rc ? error_reply(rc, rc == -ENOENT ? "no such entity" : rc == -EINVAL ? "op is rename, merge, set_kind, delete_entity or delete_relationship" : NULL) : result(type, o);
    }
    if (strcmp(type, "graph.reextract") == 0) {
        int rc = thread_store_reextract(s, owner, mc_json_string(req, "document_id"), source_of(req));
        return rc ? error_reply(rc, rc == -ENOENT ? "no such document" : NULL) : result(type, NULL);
    }
    if (strcmp(type, "policy") == 0) {
        struct json_object *set = mc_json_object(req, "set");
        if (set) {
            int rc = thread_store_policy_set(s, set);
            if (rc) return error_reply(rc, "the policy could not be read");
        }
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "policy", thread_store_policy_json(s));
        return result(type, o);
    }
    if (strcmp(type, "ledger") == 0) {
        thread_ledger_filter f = { .before_id = int_of(req, "before_id", 0), .kind = mc_json_string(req, "kind"), .document_id = mc_json_string(req, "document_id"),
                                   .request_id = mc_json_string(req, "request_id"), .limit = int_of(req, "limit", 50) };
        struct json_object *rows = NULL;
        int more = 0;
        thread_store_lock(s);
        int rc = thread_ledger_page(thread_store_db(s), &f, &rows, &more);
        thread_store_unlock(s);
        if (rc) return error_reply(rc, NULL);
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "rows", rows);
        json_object_object_add(o, "has_more", json_object_new_boolean(more));
        return result(type, o);
    }
    if (strcmp(type, "parity") == 0) return result(type, thread_store_parity_last(s, owner));
    if (strcmp(type, "parity.report") == 0) {
        int64_t run = 0;
        mc_json_int64(req, "run_id", &run);
        bool last = false;
        mc_json_bool(req, "last", &last);
        struct json_object *o = NULL;
        int rc = thread_store_parity_report(s, owner, &run, mc_json_array(req, "entries"), last, &o);
        return rc ? error_reply(rc, rc == -ENOENT ? "no such run" : NULL) : result(type, o);
    }
    if (strcmp(type, "file.record") == 0) {
        const char *key = mc_json_string(req, "path");
        if (!key) key = mc_json_string(req, "id");
        struct json_object *o = NULL;
        int rc = thread_store_file_record(s, owner, key, &o);
        return rc ? error_reply(rc, rc == -ENOENT ? "no record for that file" : NULL) : result(type, o);
    }
    if (strcmp(type, "file.move") == 0) {
        int rc = thread_store_file_move(s, owner, mc_json_string(req, "from"), mc_json_string(req, "to"), source_of(req));
        return rc ? error_reply(rc, rc == -ENOENT ? "no record for that file" : NULL) : result(type, NULL);
    }
    if (strcmp(type, "file.remove") == 0) {
        bool removed = false;
        int rc = thread_store_file_remove(s, owner, mc_json_string(req, "path") ? mc_json_string(req, "path") : "", source_of(req), &removed);
        if (rc) return error_reply(rc, NULL);
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "removed", json_object_new_boolean(removed));
        return result(type, o);
    }
    if (strcmp(type, "enrich.drain") == 0) {
        int ran = thread_store_enrich_drain(s);
        if (ran < 0) return error_reply(ran, NULL);
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "ran", json_object_new_int(ran));
        return result(type, o);
    }
    if (strcmp(type, "backup") == 0) {
        const char *path = mc_json_string(req, "path");
        if (!path || !*path) return error_reply(-EINVAL, "backup needs a path");
        int rc = thread_store_backup(s, path);
        if (rc) return error_reply(rc, "the backup could not be written");
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "path", json_object_new_string(path));
        return result(type, o);
    }
    if (strcmp(type, "checkpoint") == 0) {
        int rc = thread_store_checkpoint(s);
        return rc ? error_reply(rc, NULL) : result(type, NULL);
    }
    char message[128];
    snprintf(message, sizeof message, "threadd does not know \"%.40s\"", type);
    return error_reply(-EINVAL, message);
}

struct serving {
    thread_caller *caller;
    int fd;
    int rc;
};

static int on_line(const char *line, size_t len, void *user) {
    struct serving *sv = user;
    struct json_object *req = mc_json_parse(line, len);
    struct json_object *reply = req ? thread_local_handle(sv->caller, req) : error_reply(-EINVAL, "the line is not JSON");
    size_t n = 0;
    const char *text = mc_json_compact(reply, &n);
    int rc = mc_write_all(sv->fd, text, n);
    if (rc == 0) rc = mc_write_all(sv->fd, "\n", 1);
    json_object_put(reply);
    if (req) json_object_put(req);
    if (rc) {
        sv->rc = rc;
        return 1;
    }
    return 0;
}

int thread_local_serve(int fd, thread_caller *caller) {
    struct serving sv = { caller, fd, 0 };
    mc_line_reader reader;
    mc_line_reader_init(&reader, THREAD_LOCAL_LINE_MAX);
    char buf[65536];
    for (;;) {
        ssize_t got = read(fd, buf, sizeof buf);
        if (got < 0) {
            if (errno == EINTR) continue;
            sv.rc = -errno;
            break;
        }
        if (got == 0) break;
        int rc = mc_line_reader_feed(&reader, buf, (size_t)got, on_line, &sv);
        if (rc == 1 || sv.rc) break;
        if (rc == -EMSGSIZE) {
            struct json_object *reply = error_reply(-EINVAL, "a line was longer than 16 MiB");
            size_t n = 0;
            const char *text = mc_json_compact(reply, &n);
            mc_write_all(fd, text, n);
            mc_write_all(fd, "\n", 1);
            json_object_put(reply);
        }
    }
    mc_line_reader_free(&reader);
    return sv.rc;
}
