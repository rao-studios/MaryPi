#include "thread/proto.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "common/json.h"

static char *text_or_empty(const char *text) {
    return text && *text ? strdup(text) : (char *)protobuf_c_empty_string;
}

static void free_text(char *text) {
    if (text && text != protobuf_c_empty_string) free(text);
}

static char **strings_of(struct json_object *arr, size_t *n) {
    *n = 0;
    if (!arr || !json_object_array_length(arr)) return NULL;
    char **v = calloc(json_object_array_length(arr), sizeof *v);
    for (size_t i = 0; v && i < json_object_array_length(arr); i++) v[(*n)++] = strdup(json_object_get_string(json_object_array_get_idx(arr, i)));
    return v;
}

static void strings_free(char **v, size_t n) {
    for (size_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

/* MARK: - index */

int thread_store_index(thread_store *s, const char *owner, const Thread__V1__ThreadIndexRequest *req, size_t *indexed) {
    *indexed = 0;
    int first_error = 0;
    const char *group_id = req->group_id && *req->group_id ? req->group_id : NULL;
    if (group_id && !thread_id_valid(group_id)) return -EINVAL;
    for (size_t i = 0; i < req->n_items; i++) {
        const Thread__V1__ThreadIndexItem *item = req->items[i];
        if (item->n_texts == 0) continue;
        thread_graph_payload payload = { 0 };
        for (size_t e = 0; e < item->n_entities; e++) thread_payload_add_entity(&payload, item->entities[e]->name, item->entities[e]->kind);
        for (size_t r = 0; r < item->n_relationships; r++)
            thread_payload_add_relation(&payload, item->relationships[r]->subject, item->relationships[r]->predicate, item->relationships[r]->object);
        thread_deposit d = {
            .document_id = item->document_id,
            .group_id = group_id,
            .group_label = req->group_label,
            .texts = (const char *const *)item->texts,
            .n_texts = item->n_texts,
            .name = item->name,
            .media_type = item->media_type,
            .metadata = (const char *)item->metadata.data,
            .metadata_len = item->metadata.len,
            .tags = item->n_tags ? (const char *const *)item->tags : NULL,
            .n_tags = item->n_tags,
            .graph = payload.n_entities || payload.n_relationships ? &payload : NULL,
            .source = "grpc",
            .request_id = item->document_id,
        };
        int rc = thread_store_deposit(s, owner, &d, NULL, NULL);
        thread_payload_free(&payload);
        if (rc == 0) (*indexed)++;
        else if (!first_error) first_error = rc;
    }
    return first_error;
}

/* MARK: - library */

static Thread__V1__ThreadGroup *group_message(struct json_object *g) {
    Thread__V1__ThreadGroup *out = malloc(sizeof *out);
    thread__v1__thread_group__init(out);
    out->id = text_or_empty(mc_json_string(g, "id"));
    out->label = text_or_empty(mc_json_string(g, "label"));
    out->owner_id = text_or_empty(mc_json_string(g, "owner_id"));
    out->access = text_or_empty(mc_json_string(g, "access"));
    out->group_description = text_or_empty(mc_json_string(g, "description"));
    out->tags = strings_of(mc_json_array(g, "tags"), &out->n_tags);
    struct json_object *docs = mc_json_array(g, "documents");
    size_t n = docs ? json_object_array_length(docs) : 0;
    out->documents = n ? calloc(n, sizeof *out->documents) : NULL;
    for (size_t i = 0; i < n; i++) {
        struct json_object *d = json_object_array_get_idx(docs, i);
        Thread__V1__ThreadDocument *m = malloc(sizeof *m);
        thread__v1__thread_document__init(m);
        m->id = text_or_empty(mc_json_string(d, "id"));
        m->owner_id = text_or_empty(mc_json_string(g, "owner_id"));
        m->name = text_or_empty(mc_json_string(d, "name"));
        int64_t created = 0;
        mc_json_int64(d, "created_at", &created);
        m->created_at = created;
        out->documents[out->n_documents++] = m;
    }
    return out;
}

Thread__V1__ThreadLibraryResponse *thread_store_library(thread_store *s, const char *owner, const Thread__V1__ThreadLibraryRequest *req) {
    Thread__V1__ThreadLibraryResponse *resp = malloc(sizeof *resp);
    thread__v1__thread_library_response__init(resp);
    struct json_object *lib = thread_store_library_json(s, owner, req->limit, req->after_id, (const char *const *)req->document_ids, req->n_document_ids);
    struct json_object *groups = mc_json_array(lib, "groups");
    size_t n = groups ? json_object_array_length(groups) : 0;
    resp->groups = n ? calloc(n, sizeof *resp->groups) : NULL;
    for (size_t i = 0; i < n; i++) resp->groups[resp->n_groups++] = group_message(json_object_array_get_idx(groups, i));
    bool more = false;
    mc_json_bool(lib, "has_more", &more);
    resp->has_more = more;
    json_object_put(lib);
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
        strings_free(g->tags, g->n_tags);
        free_text(g->id);
        free_text(g->label);
        free_text(g->owner_id);
        free_text(g->access);
        free_text(g->group_description);
        free(g);
    }
    free(resp->groups);
    free(resp);
}

/* MARK: - documents */

static Thread__V1__ThreadDocumentContent *content_message(struct json_object *d) {
    Thread__V1__ThreadDocumentContent *c = malloc(sizeof *c);
    thread__v1__thread_document_content__init(c);
    c->id = text_or_empty(mc_json_string(d, "id"));
    c->name = text_or_empty(mc_json_string(d, "name"));
    c->owner_id = text_or_empty(mc_json_string(d, "owner_id"));
    c->group_id = text_or_empty(mc_json_string(d, "group_id"));
    c->group_label = text_or_empty(mc_json_string(d, "group_label"));
    int64_t created = 0;
    mc_json_int64(d, "created_at", &created);
    c->created_at = created;
    c->media_type = text_or_empty(mc_json_string(d, "media_type"));
    c->texts = strings_of(mc_json_array(d, "texts"), &c->n_texts);
    return c;
}

static void content_free(Thread__V1__ThreadDocumentContent *c) {
    strings_free(c->texts, c->n_texts);
    free_text(c->id);
    free_text(c->name);
    free_text(c->owner_id);
    free_text(c->group_id);
    free_text(c->group_label);
    free_text(c->media_type);
    free(c);
}

Thread__V1__ThreadDocumentsResponse *thread_store_documents(thread_store *s, const char *owner, const Thread__V1__ThreadDocumentsRequest *req) {
    Thread__V1__ThreadDocumentsResponse *resp = malloc(sizeof *resp);
    thread__v1__thread_documents_response__init(resp);
    struct json_object *docs = thread_store_documents_json(s, owner, (const char *const *)req->document_ids, req->n_document_ids);
    struct json_object *arr = mc_json_array(docs, "documents");
    size_t n = arr ? json_object_array_length(arr) : 0;
    resp->documents = n ? calloc(n, sizeof *resp->documents) : NULL;
    for (size_t i = 0; i < n; i++) resp->documents[resp->n_documents++] = content_message(json_object_array_get_idx(arr, i));
    json_object_put(docs);
    return resp;
}

void thread_documents_response_free(Thread__V1__ThreadDocumentsResponse *resp) {
    if (!resp) return;
    for (size_t i = 0; i < resp->n_documents; i++) content_free(resp->documents[i]);
    free(resp->documents);
    free(resp);
}

Thread__V1__ThreadExportCorpusResponse *thread_store_export(thread_store *s, const char *owner, const Thread__V1__ThreadExportCorpusRequest *req) {
    Thread__V1__ThreadExportCorpusResponse *resp = malloc(sizeof *resp);
    thread__v1__thread_export_corpus_response__init(resp);
    struct json_object *ex = thread_store_export_json(s, owner, (const char *const *)req->group_ids, req->n_group_ids, req->document_id_prefix, req->after_id, req->limit);
    struct json_object *arr = mc_json_array(ex, "documents");
    size_t n = arr ? json_object_array_length(arr) : 0;
    resp->documents = n ? calloc(n, sizeof *resp->documents) : NULL;
    for (size_t i = 0; i < n; i++) resp->documents[resp->n_documents++] = content_message(json_object_array_get_idx(arr, i));
    bool more = false;
    mc_json_bool(ex, "has_more", &more);
    resp->has_more = more;
    json_object_put(ex);
    return resp;
}

void thread_export_response_free(Thread__V1__ThreadExportCorpusResponse *resp) {
    if (!resp) return;
    for (size_t i = 0; i < resp->n_documents; i++) content_free(resp->documents[i]);
    free(resp->documents);
    free(resp);
}

/* MARK: - search */

Thread__V1__ThreadSearchResponse *thread_store_search_proto(thread_store *s, const char *owner, const Thread__V1__ThreadSearchRequest *req, int *error) {
    thread_search_request q = {
        .query_text = req->query_text,
        .query_embedding = req->n_query_embedding ? req->query_embedding : NULL,
        .dim = req->n_query_embedding,
        .entities = (const char *const *)req->entities,
        .n_entities = req->n_entities,
        .group_ids = (const char *const *)req->group_ids,
        .n_groups = req->n_group_ids,
        .aggregate = req->aggregate,
        .global = req->scope && strcmp(req->scope, "global") == 0,
        .top_k = req->top_k,
        .source = "grpc",
    };
    struct json_object *result = NULL;
    int rc = thread_store_search(s, owner, &q, &result);
    if (rc) {
        if (error) *error = rc;
        return NULL;
    }
    Thread__V1__ThreadSearchResponse *resp = malloc(sizeof *resp);
    thread__v1__thread_search_response__init(resp);
    struct json_object *results = mc_json_array(result, "results");
    size_t n = results ? json_object_array_length(results) : 0;
    resp->results = n ? calloc(n, sizeof *resp->results) : NULL;
    for (size_t i = 0; i < n; i++) {
        struct json_object *r = json_object_array_get_idx(results, i);
        Thread__V1__ThreadPartitionResult *m = malloc(sizeof *m);
        thread__v1__thread_partition_result__init(m);
        m->thread_id = text_or_empty(thread_store_node_id(s));
        m->partition_id = text_or_empty(mc_json_string(r, "partition_id"));
        m->document_id = text_or_empty(mc_json_string(r, "document_id"));
        m->owner_id = text_or_empty(mc_json_string(r, "owner_id"));
        m->text = text_or_empty(mc_json_string(r, "text"));
        double score = 0;
        mc_json_double(r, "score", &score);
        m->score = (float)score;
        resp->results[resp->n_results++] = m;
    }
    struct json_object *trace = mc_json_object(result, "trace");
    if (trace) {
        resp->trace = malloc(sizeof *resp->trace);
        thread__v1__thread_graph_trace__init(resp->trace);
        resp->trace->matched_entity_ids = strings_of(mc_json_array(trace, "matched_entity_ids"), &resp->trace->n_matched_entity_ids);
        resp->trace->expansion_edge_ids = strings_of(mc_json_array(trace, "expansion_edge_ids"), &resp->trace->n_expansion_edge_ids);
        int64_t expanded = 0;
        mc_json_int64(trace, "expanded_document_count", &expanded);
        resp->trace->expanded_document_count = (int32_t)expanded;
    }
    json_object_put(result);
    return resp;
}

void thread_search_response_free(Thread__V1__ThreadSearchResponse *resp) {
    if (!resp) return;
    for (size_t i = 0; i < resp->n_results; i++) {
        Thread__V1__ThreadPartitionResult *m = resp->results[i];
        free_text(m->thread_id);
        free_text(m->partition_id);
        free_text(m->document_id);
        free_text(m->owner_id);
        free_text(m->text);
        free(m);
    }
    free(resp->results);
    if (resp->trace) {
        strings_free(resp->trace->matched_entity_ids, resp->trace->n_matched_entity_ids);
        strings_free(resp->trace->expansion_edge_ids, resp->trace->n_expansion_edge_ids);
        free(resp->trace);
    }
    free(resp);
}

/* MARK: - graph */

Thread__V1__ThreadGraphQueryResponse *thread_store_graph_proto(thread_store *s, const char *owner, const Thread__V1__ThreadGraphQueryRequest *req) {
    thread_graph_request q = {
        .entity = req->entity && *req->entity ? req->entity : NULL,
        .query = req->query && *req->query ? req->query : NULL,
        .kinds = (const char *const *)req->kinds,
        .n_kinds = req->n_kinds,
        .hops = req->hops,
        .limit = req->limit,
        .include_documents = req->include_documents,
    };
    struct json_object *result = NULL;
    thread_store_graph_query(s, owner, &q, &result);
    Thread__V1__ThreadGraphQueryResponse *resp = malloc(sizeof *resp);
    thread__v1__thread_graph_query_response__init(resp);
    struct json_object *ents = mc_json_array(result, "entities");
    size_t n = ents ? json_object_array_length(ents) : 0;
    resp->entities = n ? calloc(n, sizeof *resp->entities) : NULL;
    for (size_t i = 0; i < n; i++) {
        struct json_object *e = json_object_array_get_idx(ents, i);
        Thread__V1__ThreadGraphEntity *m = malloc(sizeof *m);
        thread__v1__thread_graph_entity__init(m);
        m->id = text_or_empty(mc_json_string(e, "id"));
        m->name = text_or_empty(mc_json_string(e, "name"));
        m->kind = text_or_empty(mc_json_string(e, "kind"));
        double score = 0;
        mc_json_double(e, "score", &score);
        m->score = (float)score;
        int64_t mentions = 0;
        mc_json_int64(e, "mention_count", &mentions);
        m->mention_count = (int32_t)mentions;
        m->document_ids = strings_of(mc_json_array(e, "document_ids"), &m->n_document_ids);
        resp->entities[resp->n_entities++] = m;
    }
    struct json_object *rels = mc_json_array(result, "relationships");
    n = rels ? json_object_array_length(rels) : 0;
    resp->relationships = n ? calloc(n, sizeof *resp->relationships) : NULL;
    for (size_t i = 0; i < n; i++) {
        struct json_object *r = json_object_array_get_idx(rels, i);
        Thread__V1__ThreadGraphRelationship *m = malloc(sizeof *m);
        thread__v1__thread_graph_relationship__init(m);
        m->id = text_or_empty(mc_json_string(r, "id"));
        m->subject_id = text_or_empty(mc_json_string(r, "subject_id"));
        m->predicate = text_or_empty(mc_json_string(r, "predicate"));
        m->object_id = text_or_empty(mc_json_string(r, "object_id"));
        int64_t weight = 0;
        mc_json_int64(r, "weight", &weight);
        m->weight = (int32_t)weight;
        m->document_ids = strings_of(mc_json_array(r, "document_ids"), &m->n_document_ids);
        resp->relationships[resp->n_relationships++] = m;
    }
    struct json_object *docs = mc_json_array(result, "documents");
    n = docs ? json_object_array_length(docs) : 0;
    resp->documents = n ? calloc(n, sizeof *resp->documents) : NULL;
    for (size_t i = 0; i < n; i++) {
        struct json_object *d = json_object_array_get_idx(docs, i);
        Thread__V1__ThreadGraphDocument *m = malloc(sizeof *m);
        thread__v1__thread_graph_document__init(m);
        m->id = text_or_empty(mc_json_string(d, "id"));
        m->name = text_or_empty(mc_json_string(d, "name"));
        m->owner_id = text_or_empty(mc_json_string(d, "owner_id"));
        resp->documents[resp->n_documents++] = m;
    }
    resp->stats = malloc(sizeof *resp->stats);
    thread__v1__thread_graph_stats__init(resp->stats);
    int64_t v = 0;
    mc_json_int64(result, "entity_count", &v);
    resp->stats->entity_count = v;
    v = 0;
    mc_json_int64(result, "relationship_count", &v);
    resp->stats->relationship_count = v;
    json_object_put(result);
    return resp;
}

void thread_graph_response_free(Thread__V1__ThreadGraphQueryResponse *resp) {
    if (!resp) return;
    for (size_t i = 0; i < resp->n_entities; i++) {
        Thread__V1__ThreadGraphEntity *m = resp->entities[i];
        free_text(m->id);
        free_text(m->name);
        free_text(m->kind);
        strings_free(m->document_ids, m->n_document_ids);
        free(m);
    }
    free(resp->entities);
    for (size_t i = 0; i < resp->n_relationships; i++) {
        Thread__V1__ThreadGraphRelationship *m = resp->relationships[i];
        free_text(m->id);
        free_text(m->subject_id);
        free_text(m->predicate);
        free_text(m->object_id);
        strings_free(m->document_ids, m->n_document_ids);
        free(m);
    }
    free(resp->relationships);
    for (size_t i = 0; i < resp->n_documents; i++) {
        Thread__V1__ThreadGraphDocument *m = resp->documents[i];
        free_text(m->id);
        free_text(m->name);
        free_text(m->owner_id);
        free(m);
    }
    free(resp->documents);
    free(resp->stats);
    free(resp);
}
