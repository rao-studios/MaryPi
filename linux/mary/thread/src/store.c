#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "thread/store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "common/base64.h"
#include "common/io.h"
#include "common/json.h"

#define FILE_MAX (8u << 20)

struct thread_store {
    char dir[512];
    char node_id[37];
    pthread_mutex_t lock;
};

bool thread_id_valid(const char *id) {
    if (!id) return false;
    size_t n = strlen(id);
    if (n == 0 || n > THREAD_ID_MAX || strcmp(id, ".") == 0 || strcmp(id, "..") == 0) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' || c == '-'))
            return false;
    }
    return true;
}

static void path_of(const thread_store *s, const char *kind, const char *id, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s/%s.json", s->dir, kind, id);
}

static struct json_object *load(const char *path) {
    mc_buf b = { 0 };
    struct json_object *obj = mc_read_file(path, &b, FILE_MAX) == 0 ? mc_json_parse((const char *)b.data, b.len) : NULL;
    mc_buf_free(&b);
    return obj;
}

static int save(const char *path, struct json_object *obj) {
    size_t len = 0;
    const char *text = mc_json_compact(obj, &len);
    return text ? mc_write_file_atomic(path, text, len, 0600) : -ENOMEM;
}

static int new_uuid(char *out) {
    unsigned char b[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -errno;
    ssize_t got = read(fd, b, sizeof b);
    close(fd);
    if (got != (ssize_t)sizeof b) return -EIO;
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return 0;
}

thread_store *thread_store_open(const char *dir, int *error) {
    thread_store *s = calloc(1, sizeof *s);
    int rc = s ? 0 : -ENOMEM;
    if (s) snprintf(s->dir, sizeof s->dir, "%s", dir && *dir ? dir : THREAD_STATE_DIR);
    char path[600];
    const char *const subdirs[] = { "", "/groups", "/documents" };
    for (int i = 0; rc == 0 && i < 3; i++) {
        snprintf(path, sizeof path, "%s%s", s->dir, subdirs[i]);
        if (mkdir(path, 0700) < 0 && errno != EEXIST) rc = -errno;
    }
    if (rc == 0) {
        snprintf(path, sizeof path, "%s/node-id", s->dir);
        mc_buf b = { 0 };
        if (mc_read_file(path, &b, 64) == 0 && b.len >= 36) {
            memcpy(s->node_id, b.data, 36);
            s->node_id[36] = 0;
        } else if ((rc = new_uuid(s->node_id)) == 0) {
            char line[38];
            snprintf(line, sizeof line, "%s\n", s->node_id);
            rc = mc_write_file_atomic(path, line, 37, 0600);
        }
        mc_buf_free(&b);
    }
    if (rc) {
        free(s);
        if (error) *error = rc;
        return NULL;
    }
    pthread_mutex_init(&s->lock, NULL);
    return s;
}

void thread_store_close(thread_store *s) {
    if (!s) return;
    pthread_mutex_destroy(&s->lock);
    free(s);
}

const char *thread_store_node_id(const thread_store *s) { return s->node_id; }

/* MARK: - Index */

static bool owned_by(struct json_object *obj, const char *owner) {
    const char *o = mc_json_string(obj, "owner_id");
    return o && strcmp(o, owner) == 0;
}

static void remove_from_group(thread_store *s, const char *group_id, const char *document_id) {
    char path[600];
    path_of(s, "groups", group_id, path, sizeof path);
    struct json_object *group = load(path);
    struct json_object *docs = mc_json_array(group, "documents"), *kept = json_object_new_array();
    for (size_t i = 0; docs && i < json_object_array_length(docs); i++) {
        struct json_object *id = json_object_array_get_idx(docs, i);
        if (strcmp(json_object_get_string(id), document_id) != 0) json_object_array_add(kept, json_object_get(id));
    }
    if (group) {
        json_object_object_add(group, "documents", kept);
        save(path, group);
    } else {
        json_object_put(kept);
    }
    json_object_put(group);
}

int thread_store_index(thread_store *s, const char *owner, const Thread__V1__ThreadIndexRequest *req, size_t *indexed) {
    *indexed = 0;
    const char *group_id = req->group_id && *req->group_id ? req->group_id : NULL;
    if (group_id && !thread_id_valid(group_id)) return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = 0;
    char path[600];
    struct json_object *group = NULL;
    if (group_id) {
        path_of(s, "groups", group_id, path, sizeof path);
        group = load(path);
        if (group && !owned_by(group, owner)) rc = -EPERM;
    }
    /* Check every item before writing any. */
    for (size_t i = 0; rc == 0 && i < req->n_items; i++) {
        const Thread__V1__ThreadIndexItem *item = req->items[i];
        if (item->n_texts == 0) continue;
        if (!thread_id_valid(item->document_id)) {
            rc = -EINVAL;
            break;
        }
        path_of(s, "documents", item->document_id, path, sizeof path);
        struct json_object *existing = load(path);
        if (existing && !owned_by(existing, owner)) rc = -EPERM;
        json_object_put(existing);
    }
    int64_t now = (int64_t)time(NULL);
    if (rc == 0 && group_id && !group) {
        group = json_object_new_object();
        json_object_object_add(group, "id", json_object_new_string(group_id));
        json_object_object_add(group, "label", json_object_new_string(""));
        json_object_object_add(group, "owner_id", json_object_new_string(owner));
        json_object_object_add(group, "created_at", json_object_new_int64(now));
        json_object_object_add(group, "documents", json_object_new_array());
    }
    if (rc == 0 && group && req->group_label && *req->group_label)
        json_object_object_add(group, "label", json_object_new_string(req->group_label));

    for (size_t i = 0; rc == 0 && i < req->n_items; i++) {
        const Thread__V1__ThreadIndexItem *item = req->items[i];
        if (item->n_texts == 0) continue;
        path_of(s, "documents", item->document_id, path, sizeof path);
        struct json_object *existing = load(path);
        int64_t created = now;
        mc_json_int64(existing, "created_at", &created);
        const char *old_group = mc_json_string(existing, "group_id");
        if (old_group && *old_group && (!group_id || strcmp(old_group, group_id) != 0))
            remove_from_group(s, old_group, item->document_id);
        json_object_put(existing);

        struct json_object *doc = json_object_new_object(), *texts = json_object_new_array();
        json_object_object_add(doc, "id", json_object_new_string(item->document_id));
        json_object_object_add(doc, "owner_id", json_object_new_string(owner));
        json_object_object_add(doc, "group_id", json_object_new_string(group_id ? group_id : ""));
        json_object_object_add(doc, "name", json_object_new_string(item->name ? item->name : ""));
        json_object_object_add(doc, "media_type", json_object_new_string(item->media_type && *item->media_type ? item->media_type : "text"));
        json_object_object_add(doc, "created_at", json_object_new_int64(created));
        size_t b64_len = 0;
        char *b64 = mc_base64_encode_alloc(item->metadata.data, item->metadata.len, &b64_len);
        json_object_object_add(doc, "metadata", json_object_new_string(b64 ? b64 : ""));
        free(b64);
        for (size_t t = 0; t < item->n_texts; t++) json_object_array_add(texts, json_object_new_string(item->texts[t]));
        json_object_object_add(doc, "texts", texts);
        rc = save(path, doc);
        json_object_put(doc);
        if (rc) break;
        (*indexed)++;

        if (group) {
            struct json_object *docs = mc_json_array(group, "documents");
            bool present = false;
            for (size_t d = 0; docs && d < json_object_array_length(docs); d++)
                if (strcmp(json_object_get_string(json_object_array_get_idx(docs, d)), item->document_id) == 0) present = true;
            if (!present && docs) json_object_array_add(docs, json_object_new_string(item->document_id));
        }
    }
    if (rc == 0 && group) {
        path_of(s, "groups", group_id, path, sizeof path);
        rc = save(path, group);
    }
    json_object_put(group);
    pthread_mutex_unlock(&s->lock);
    return rc;
}

/* MARK: - Reading */

static char *text_or_empty(const char *text) {
    return text && *text ? strdup(text) : (char *)protobuf_c_empty_string;
}

static void free_text(char *text) {
    if (text && text != protobuf_c_empty_string) free(text);
}

static int compare_ids(const void *a, const void *b) {
    return strcmp(mc_json_string(*(struct json_object *const *)a, "id"), mc_json_string(*(struct json_object *const *)b, "id"));
}

/* The owner's groups, sorted by id; the caller puts each and frees the array. */
static struct json_object **owners_groups(thread_store *s, const char *owner, size_t *count) {
    char path[600];
    snprintf(path, sizeof path, "%s/groups", s->dir);
    *count = 0;
    DIR *dir = opendir(path);
    if (!dir) return NULL;
    struct json_object **groups = NULL;
    size_t cap = 0;
    struct dirent *e;
    while ((e = readdir(dir))) {
        size_t n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 5, ".json") != 0) continue;
        snprintf(path, sizeof path, "%s/groups/%s", s->dir, e->d_name);
        struct json_object *g = load(path);
        if (!g || !owned_by(g, owner) || !mc_json_string(g, "id")) {
            json_object_put(g);
            continue;
        }
        if (*count == cap) {
            cap = cap ? cap * 2 : 16;
            groups = realloc(groups, cap * sizeof *groups);
        }
        groups[(*count)++] = g;
    }
    closedir(dir);
    if (*count) qsort(groups, *count, sizeof *groups, compare_ids);
    return groups;
}

static Thread__V1__ThreadGroup *group_message(thread_store *s, struct json_object *g) {
    Thread__V1__ThreadGroup *out = malloc(sizeof *out);
    thread__v1__thread_group__init(out);
    out->id = text_or_empty(mc_json_string(g, "id"));
    out->label = text_or_empty(mc_json_string(g, "label"));
    out->owner_id = text_or_empty(mc_json_string(g, "owner_id"));
    struct json_object *docs = mc_json_array(g, "documents");
    size_t n = docs ? json_object_array_length(docs) : 0;
    out->documents = n ? calloc(n, sizeof *out->documents) : NULL;
    for (size_t i = 0; i < n; i++) {
        const char *id = json_object_get_string(json_object_array_get_idx(docs, i));
        char path[600];
        path_of(s, "documents", id, path, sizeof path);
        struct json_object *doc = thread_id_valid(id) ? load(path) : NULL;
        Thread__V1__ThreadDocument *d = malloc(sizeof *d);
        thread__v1__thread_document__init(d);
        d->id = text_or_empty(id);
        d->owner_id = text_or_empty(mc_json_string(g, "owner_id"));
        d->name = text_or_empty(mc_json_string(doc, "name"));
        int64_t created = 0;
        mc_json_int64(doc, "created_at", &created);
        d->created_at = created;
        json_object_put(doc);
        out->documents[out->n_documents++] = d;
    }
    return out;
}

static bool group_holds_any(struct json_object *g, char *const *ids, size_t n) {
    struct json_object *docs = mc_json_array(g, "documents");
    for (size_t d = 0; docs && d < json_object_array_length(docs); d++) {
        const char *doc = json_object_get_string(json_object_array_get_idx(docs, d));
        for (size_t i = 0; i < n; i++) if (strcmp(doc, ids[i]) == 0) return true;
    }
    return false;
}

Thread__V1__ThreadLibraryResponse *thread_store_library(thread_store *s, const char *owner, const Thread__V1__ThreadLibraryRequest *req) {
    Thread__V1__ThreadLibraryResponse *resp = malloc(sizeof *resp);
    thread__v1__thread_library_response__init(resp);
    pthread_mutex_lock(&s->lock);
    size_t count = 0;
    struct json_object **groups = owners_groups(s, owner, &count);
    resp->groups = count ? calloc(count, sizeof *resp->groups) : NULL;
    const char *after = req->after_id ? req->after_id : "";
    size_t limit = req->limit > 0 ? (size_t)req->limit : 0;
    for (size_t i = 0; i < count; i++) {
        if (req->n_document_ids) {
            if (group_holds_any(groups[i], req->document_ids, req->n_document_ids))
                resp->groups[resp->n_groups++] = group_message(s, groups[i]);
            continue;
        }
        if (*after && strcmp(mc_json_string(groups[i], "id"), after) <= 0) continue;
        if (limit && resp->n_groups == limit) {
            resp->has_more = 1;
            break;
        }
        resp->groups[resp->n_groups++] = group_message(s, groups[i]);
    }
    for (size_t i = 0; i < count; i++) json_object_put(groups[i]);
    free(groups);
    pthread_mutex_unlock(&s->lock);
    return resp;
}

void thread_library_response_free(Thread__V1__ThreadLibraryResponse *resp) {
    if (!resp) return;
    for (size_t i = 0; i < resp->n_groups; i++) {
        Thread__V1__ThreadGroup *g = resp->groups[i];
        for (size_t d = 0; d < g->n_documents; d++) {
            free_text(g->documents[d]->id);
            free_text(g->documents[d]->owner_id);
            free_text(g->documents[d]->name);
            free(g->documents[d]);
        }
        free(g->documents);
        free_text(g->id);
        free_text(g->label);
        free_text(g->owner_id);
        free(g);
    }
    free(resp->groups);
    free(resp);
}

Thread__V1__ThreadDocumentsResponse *thread_store_documents(thread_store *s, const char *owner, const Thread__V1__ThreadDocumentsRequest *req) {
    Thread__V1__ThreadDocumentsResponse *resp = malloc(sizeof *resp);
    thread__v1__thread_documents_response__init(resp);
    resp->documents = req->n_document_ids ? calloc(req->n_document_ids, sizeof *resp->documents) : NULL;
    pthread_mutex_lock(&s->lock);
    for (size_t i = 0; i < req->n_document_ids; i++) {
        const char *id = req->document_ids[i];
        if (!thread_id_valid(id)) continue;
        char path[600];
        path_of(s, "documents", id, path, sizeof path);
        struct json_object *doc = load(path);
        if (!doc || !owned_by(doc, owner)) {
            json_object_put(doc);
            continue;
        }
        Thread__V1__ThreadDocumentContent *c = malloc(sizeof *c);
        thread__v1__thread_document_content__init(c);
        c->id = text_or_empty(id);
        c->name = text_or_empty(mc_json_string(doc, "name"));
        c->owner_id = text_or_empty(owner);
        const char *group_id = mc_json_string(doc, "group_id");
        c->group_id = text_or_empty(group_id);
        if (group_id && thread_id_valid(group_id)) {
            path_of(s, "groups", group_id, path, sizeof path);
            struct json_object *g = load(path);
            c->group_label = text_or_empty(mc_json_string(g, "label"));
            json_object_put(g);
        } else {
            c->group_label = (char *)protobuf_c_empty_string;
        }
        int64_t created = 0;
        mc_json_int64(doc, "created_at", &created);
        c->created_at = created;
        c->media_type = text_or_empty(mc_json_string(doc, "media_type"));
        struct json_object *texts = mc_json_array(doc, "texts");
        size_t n = texts ? json_object_array_length(texts) : 0;
        c->texts = n ? calloc(n, sizeof *c->texts) : NULL;
        for (size_t t = 0; t < n; t++) c->texts[c->n_texts++] = strdup(json_object_get_string(json_object_array_get_idx(texts, t)));
        json_object_put(doc);
        resp->documents[resp->n_documents++] = c;
    }
    pthread_mutex_unlock(&s->lock);
    return resp;
}

void thread_documents_response_free(Thread__V1__ThreadDocumentsResponse *resp) {
    if (!resp) return;
    for (size_t i = 0; i < resp->n_documents; i++) {
        Thread__V1__ThreadDocumentContent *c = resp->documents[i];
        for (size_t t = 0; t < c->n_texts; t++) free(c->texts[t]);
        free(c->texts);
        free_text(c->id);
        free_text(c->name);
        free_text(c->owner_id);
        free_text(c->group_id);
        free_text(c->group_label);
        free_text(c->media_type);
        free(c);
    }
    free(resp->documents);
    free(resp);
}
