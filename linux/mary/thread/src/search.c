/* Search: Thread's ThreadQueryServiceImpl.search and PartitionTable.search, and the
 * graph query (ThreadGraphServiceImpl.query, Database+Graph.swift). */
#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/io.h"
#include "common/json.h"
#include "common/log.h"
#include "thread/families.h"
#include "thread/ledger.h"
#include "thread/text.h"

/* MARK: - the query cache */

const float *thread_store_query_vector(thread_store *s, const char *text, char *message, size_t cap) {
    if (!text || !*text) {
        snprintf(message, cap, "nothing to embed");
        return NULL;
    }
    for (int i = 0; i < THREAD_QUERY_CACHE; i++) {
        if (s->queries[i].text && strcmp(s->queries[i].text, text) == 0) {
            s->queries[i].used = ++s->query_clock;
            return s->queries[i].vector;
        }
    }
    if (!s->embedder || !s->embedder->embed) {
        snprintf(message, cap, "no embedder: sewnd is not configured");
        return NULL;
    }
    float *vector = malloc(s->dim * sizeof *vector);
    if (!vector) return NULL;
    const char *texts[1] = { text };
    int rc = thread_embedder_embed(s->embedder, texts, 1, vector, message, cap);
    if (rc) {
        free(vector);
        return NULL;
    }
    int slot = 0;
    for (int i = 0; i < THREAD_QUERY_CACHE; i++) {
        if (!s->queries[i].text) {
            slot = i;
            break;
        }
        if (s->queries[i].used < s->queries[slot].used) slot = i;
    }
    free(s->queries[slot].text);
    free(s->queries[slot].vector);
    s->queries[slot].text = strdup(text);
    s->queries[slot].vector = vector;
    s->queries[slot].used = ++s->query_clock;
    return vector;
}

/* MARK: - candidates */

static bool family_in_lanes(const char *family, const char *const *lanes, size_t n_lanes) {
    if (!n_lanes) return true;
    const char *lane = thread_family_lane(family);
    for (size_t i = 0; i < n_lanes; i++) if (strcmp(lanes[i], lane) == 0) return true;
    return false;
}

int thread_store_candidates(thread_store *s, const char *owner, const char *const *group_ids, size_t n_groups,
                            const char *const *lanes, size_t n_lanes, thread_ids *out) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, "SELECT id, group_id, family FROM documents WHERE owner = ?", -1, &stmt, NULL) != SQLITE_OK) return -EIO;
    thread_db_bind_text(stmt, 1, owner);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        const char *group = (const char *)sqlite3_column_text(stmt, 1);
        const char *family = (const char *)sqlite3_column_text(stmt, 2);
        if (n_groups) {
            bool in = false;
            for (size_t i = 0; !in && group && i < n_groups; i++) in = strcmp(group_ids[i], group) == 0;
            if (!in) continue;
        }
        if (!family_in_lanes(family ? family : "unknown", lanes, n_lanes)) continue;
        thread_ids_add(out, id);
    }
    sqlite3_finalize(stmt);
    return 0;
}

/* MARK: - search */

typedef struct hit {
    int64_t rowid;
    float score;
    char document_id[THREAD_ID_MAX + 1];
} hit;

static int compare_hits(const void *a, const void *b) {
    const hit *x = a, *y = b;
    if (x->score != y->score) return x->score < y->score ? -1 : 1;
    return strcmp(x->document_id, y->document_id);
}

static void add_ids(struct json_object *o, const char *key, const thread_ids *ids) {
    struct json_object *arr = json_object_new_array();
    for (size_t i = 0; i < ids->n; i++) json_object_array_add(arr, json_object_new_string(ids->v[i]));
    json_object_object_add(o, key, arr);
}

int thread_store_search(thread_store *s, const char *owner, const thread_search_request *q, struct json_object **result) {
    *result = NULL;
    int64_t started = mc_now_ms();
    int k = THREAD_SEARCH_K;
    int top_k = q->top_k > 0 && q->top_k <= THREAD_SEARCH_MAX ? q->top_k : THREAD_SEARCH_MAX;
    pthread_mutex_lock(&s->lock);
    char message[256] = "";
    const float *query = NULL;
    float *own_query = NULL;
    if (q->query_embedding && q->dim == s->dim) query = q->query_embedding;
    else if (q->query_text && *q->query_text) query = thread_store_query_vector(s, q->query_text, message, sizeof message);
    if (!query) {
        pthread_mutex_unlock(&s->lock);
        mc_log(MC_LOG_WARNING, "search: no query vector: %s", message);
        return s->embedder ? -EIO : -ENOSYS;
    }
    /* the relationship hints, embedded separately */
    const float *hint = NULL;
    if (q->n_entities) {
        char joined[1024] = "Relationship predicates: ";
        thread_ids sorted = { 0 };
        for (size_t i = 0; i < q->n_entities; i++) {
            char *t = thread_lower_trim(q->entities[i]);
            if (t && *t) thread_ids_add(&sorted, q->entities[i]);
            free(t);
        }
        for (size_t i = 0; i < sorted.n; i++)
            for (size_t j = i + 1; j < sorted.n; j++)
                if (strcmp(sorted.v[j], sorted.v[i]) < 0) { char *t = sorted.v[i]; sorted.v[i] = sorted.v[j]; sorted.v[j] = t; }
        for (size_t i = 0; i < sorted.n; i++) {
            size_t l = strlen(joined);
            snprintf(joined + l, sizeof joined - l, "%s%s", i ? ", " : "", sorted.v[i]);
        }
        if (sorted.n) hint = thread_store_query_vector(s, joined, message, sizeof message);
        thread_ids_free(&sorted);
    }
    /* graph matching */
    thread_ids matched_entities = { 0 }, matched_relationships = { 0 }, matched_predicates = { 0 };
    bool graph_nonempty = thread_graph_entity_count(s->db) > 0;
    if (graph_nonempty) {
        size_t len = strlen(q->query_text ? q->query_text : "") + 1;
        for (size_t i = 0; i < q->n_entities; i++) len += strlen(q->entities[i]) + 1;
        char *entity_query = malloc(len + 1);
        if (entity_query) {
            snprintf(entity_query, len + 1, "%s", q->query_text ? q->query_text : "");
            for (size_t i = 0; i < q->n_entities; i++) {
                strcat(entity_query, " ");
                strcat(entity_query, q->entities[i]);
            }
            thread_match *m = NULL;
            size_t n = 0;
            thread_graph_match_entities(s->db, entity_query, NULL, 0, THREAD_ENTITY_MATCH_LIMIT, &m, &n);
            for (size_t i = 0; i < n; i++) thread_ids_add(&matched_entities, m[i].id);
            free(m);
            free(entity_query);
        }
        thread_match *rels = NULL;
        size_t n = 0;
        thread_graph_match_relationships(s->db, query, s->dim, &matched_entities, THREAD_RELATIONSHIP_MATCH_LIMIT, &rels, &n);
        for (size_t i = 0; i < n; i++) {
            thread_ids_add(&matched_relationships, rels[i].id);
            thread_ids_add(&matched_predicates, rels[i].predicate_id);
        }
        free(rels);
        if (hint) {
            thread_graph_match_relationships(s->db, hint, s->dim, NULL, THREAD_RELATIONSHIP_MATCH_LIMIT, &rels, &n);
            for (size_t i = 0; i < n; i++) {
                thread_ids_add(&matched_relationships, rels[i].id);
                thread_ids_add(&matched_predicates, rels[i].predicate_id);
            }
            free(rels);
        }
    }
    /* candidates */
    thread_ids candidates = { 0 };
    thread_store_candidates(s, owner, q->group_ids, q->n_groups, q->lanes, q->n_lanes, &candidates);
    thread_ids scoped = { 0 };
    if (q->n_groups) for (size_t i = 0; i < candidates.n; i++) thread_ids_add(&scoped, candidates.v[i]);
    if (matched_relationships.n) {
        thread_ids linked = { 0 };
        thread_graph_documents_of_relationships(s->db, &matched_relationships, &linked);
        thread_ids kept = { 0 };
        for (size_t i = 0; i < candidates.n; i++) if (thread_ids_has(&linked, candidates.v[i])) thread_ids_add(&kept, candidates.v[i]);
        thread_ids_free(&candidates);
        thread_ids_free(&linked);
        candidates = kept;
    } else if (matched_entities.n) {
        thread_ids linked = { 0 };
        thread_graph_documents_of_entities(s->db, &matched_entities, &linked);
        thread_ids kept = { 0 };
        for (size_t i = 0; i < candidates.n; i++) if (thread_ids_has(&linked, candidates.v[i])) thread_ids_add(&kept, candidates.v[i]);
        thread_ids_free(&candidates);
        thread_ids_free(&linked);
        candidates = kept;
    }
    /* the scan */
    hit *hits = NULL;
    size_t n_hits = 0;
    thread_ids result_docs = { 0 };
    if (candidates.n) {
        thread_hit *raw = calloc(candidates.n * (size_t)k, sizeof *raw);
        int *counts = calloc(candidates.n, sizeof *counts);
        thread_vectors_scan(s->vectors, query, (const char *const *)candidates.v, candidates.n, k, THREAD_DISTANCE_THRESHOLD, raw, counts);
        for (size_t d = 0; d < candidates.n; d++) {
            for (int i = 0; i < counts[d]; i++) {
                hit *grown = realloc(hits, (n_hits + 1) * sizeof *grown);
                if (!grown) break;
                hits = grown;
                hits[n_hits].rowid = raw[d * (size_t)k + i].rowid;
                hits[n_hits].score = raw[d * (size_t)k + i].score;
                snprintf(hits[n_hits].document_id, sizeof hits[n_hits].document_id, "%s", candidates.v[d]);
                n_hits++;
                thread_ids_add(&result_docs, candidates.v[d]);
            }
        }
        free(raw);
        free(counts);
    }
    /* the trace, and the one-hop expansion */
    struct json_object *trace = NULL;
    if (matched_entities.n || matched_relationships.n) {
        trace = json_object_new_object();
        add_ids(trace, "matched_entity_ids", &matched_entities);
        add_ids(trace, "matched_relationship_ids", &matched_relationships);
        add_ids(trace, "matched_predicate_ids", &matched_predicates);
        json_object_object_add(trace, "expansion_edge_ids", json_object_new_array());
        json_object_object_add(trace, "expanded_document_count", json_object_new_int(0));
    }
    if (graph_nonempty) {
        thread_ids seeds = { 0 };
        for (size_t i = 0; i < matched_entities.n; i++) thread_ids_add(&seeds, matched_entities.v[i]);
        thread_graph_endpoints(s->db, &matched_relationships, &seeds);
        for (size_t i = 0; i < result_docs.n; i++) {
            sqlite3_stmt *stmt = NULL;
            if (sqlite3_prepare_v2(s->db, "SELECT entity_id FROM entity_documents WHERE document_id = ? ORDER BY position", -1, &stmt, NULL) == SQLITE_OK) {
                thread_db_bind_text(stmt, 1, result_docs.v[i]);
                while (sqlite3_step(stmt) == SQLITE_ROW) thread_ids_add(&seeds, (const char *)sqlite3_column_text(stmt, 0));
                sqlite3_finalize(stmt);
            }
        }
        if (seeds.n) {
            thread_ids nbr_entities = { 0 }, nbr_edges = { 0 }, nbr_docs = { 0 }, accessible = { 0 };
            thread_graph_neighborhood(s->db, &seeds, 1, &nbr_entities, &nbr_edges);
            thread_graph_documents_of_entities(s->db, &nbr_entities, &nbr_docs);
            thread_store_candidates(s, owner, NULL, 0, q->lanes, q->n_lanes, &accessible);
            /* neighbour documents: not already returned, accessible, and inside the scope when groups were named */
            struct ranked { char *id; int weight; } *ranked = NULL;
            size_t n_ranked = 0;
            for (size_t i = 0; i < nbr_docs.n; i++) {
                const char *doc = nbr_docs.v[i];
                if (thread_ids_has(&result_docs, doc) || !thread_ids_has(&accessible, doc)) continue;
                if (q->n_groups && !thread_ids_has(&scoped, doc)) continue;
                ranked = realloc(ranked, (n_ranked + 1) * sizeof *ranked);
                ranked[n_ranked].id = nbr_docs.v[i];
                ranked[n_ranked].weight = thread_graph_edge_weight_for(s->db, doc, &nbr_edges);
                n_ranked++;
            }
            for (size_t i = 0; i < n_ranked; i++)
                for (size_t j = i + 1; j < n_ranked; j++)
                    if (ranked[j].weight > ranked[i].weight) { struct ranked t = ranked[i]; ranked[i] = ranked[j]; ranked[j] = t; }
            int expanded = 0;
            for (size_t i = 0; i < n_ranked && i < (size_t)(2 * k); i++) {
                thread_hit best;
                if (!thread_vectors_best(s->vectors, query, ranked[i].id, &best)) continue;
                bool seen = false;
                for (size_t h = 0; h < n_hits; h++) if (hits[h].rowid == best.rowid) seen = true;
                if (seen) continue;
                hit *grown = realloc(hits, (n_hits + 1) * sizeof *grown);
                if (!grown) break;
                hits = grown;
                hits[n_hits].rowid = best.rowid;
                hits[n_hits].score = best.score * THREAD_EXPANSION_PENALTY;
                snprintf(hits[n_hits].document_id, sizeof hits[n_hits].document_id, "%s", ranked[i].id);
                n_hits++;
                expanded++;
            }
            if (!trace) {
                trace = json_object_new_object();
                add_ids(trace, "matched_entity_ids", &matched_entities);
                add_ids(trace, "matched_relationship_ids", &matched_relationships);
                add_ids(trace, "matched_predicate_ids", &matched_predicates);
            }
            json_object_object_del(trace, "expansion_edge_ids");
            add_ids(trace, "expansion_edge_ids", &nbr_edges);
            json_object_object_del(trace, "expanded_document_count");
            json_object_object_add(trace, "expanded_document_count", json_object_new_int(expanded));
            free(ranked);
            thread_ids_free(&nbr_entities);
            thread_ids_free(&nbr_edges);
            thread_ids_free(&nbr_docs);
            thread_ids_free(&accessible);
        }
        thread_ids_free(&seeds);
    }
    /* resolve, sort, cut */
    if (n_hits) qsort(hits, n_hits, sizeof *hits, compare_hits);
    if (n_hits > (size_t)top_k) n_hits = (size_t)top_k;
    struct json_object *results = json_object_new_array();
    for (size_t i = 0; i < n_hits; i++) {
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(s->db, "SELECT p.id, p.text, d.owner, d.family, d.group_id, d.name FROM partitions p JOIN documents d ON d.id = p.document_id WHERE p.rowid = ?", -1, &stmt, NULL) != SQLITE_OK) continue;
        sqlite3_bind_int64(stmt, 1, hits[i].rowid);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            struct json_object *r = json_object_new_object();
            const char *pid = (const char *)sqlite3_column_text(stmt, 0);
            json_object_object_add(r, "partition_id", json_object_new_string(pid ? pid : ""));
            json_object_object_add(r, "document_id", json_object_new_string(hits[i].document_id));
            json_object_object_add(r, "owner_id", json_object_new_string((const char *)sqlite3_column_text(stmt, 2)));
            json_object_object_add(r, "text", json_object_new_string((const char *)sqlite3_column_text(stmt, 1)));
            json_object_object_add(r, "score", json_object_new_double(hits[i].score));
            const char *family = (const char *)sqlite3_column_text(stmt, 3);
            json_object_object_add(r, "family", json_object_new_string(family ? family : "unknown"));
            json_object_object_add(r, "lane", json_object_new_string(thread_family_lane(family ? family : "unknown")));
            const char *group = (const char *)sqlite3_column_text(stmt, 4);
            json_object_object_add(r, "group_id", json_object_new_string(group ? group : ""));
            const char *name = (const char *)sqlite3_column_text(stmt, 5);
            json_object_object_add(r, "name", json_object_new_string(name ? name : ""));
            json_object_array_add(results, r);
        }
        sqlite3_finalize(stmt);
    }
    int64_t ms = mc_now_ms() - started;
    /* the ledger and the stats */
    struct json_object *detail = json_object_new_object();
    json_object_object_add(detail, "candidates", json_object_new_int64((int64_t)candidates.n));
    json_object_object_add(detail, "query_len", json_object_new_int64((int64_t)(q->query_text ? strlen(q->query_text) : 0)));
    json_object_object_add(detail, "top_k", json_object_new_int(top_k));
    struct json_object *lanes = json_object_new_array();
    for (size_t i = 0; i < q->n_lanes; i++) json_object_array_add(lanes, json_object_new_string(q->lanes[i]));
    json_object_object_add(detail, "lanes", lanes);
    struct json_object *groups = json_object_new_array();
    for (size_t i = 0; i < q->n_groups; i++) json_object_array_add(groups, json_object_new_string(q->group_ids[i]));
    json_object_object_add(detail, "groups", groups);
    if (trace) json_object_object_add(detail, "trace", json_object_get(trace));
    thread_ledger_row row = { "search", q->source ? q->source : "grpc", NULL, NULL, q->request_id, (int64_t)n_hits, ms, detail };
    int64_t ledger_id = thread_ledger_record(s->db, &row);
    json_object_put(detail);
    for (size_t i = 0; i < n_hits; i++) {
        struct json_object *r = json_object_array_get_idx(results, i);
        if (ledger_id > 0 && r) thread_ledger_record_hit(s->db, ledger_id, (int)i, hits[i].document_id, mc_json_string(r, "partition_id"), hits[i].score);
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(s->db, "INSERT INTO document_stats(document_id, retrieval_count, last_retrieved_ms) VALUES(?, 1, ?) "
                                      "ON CONFLICT(document_id) DO UPDATE SET retrieval_count = retrieval_count + 1, last_retrieved_ms = excluded.last_retrieved_ms", -1, &stmt, NULL) == SQLITE_OK) {
            thread_db_bind_text(stmt, 1, hits[i].document_id);
            sqlite3_bind_int64(stmt, 2, mc_wall_ms());
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    pthread_mutex_unlock(&s->lock);
    *result = json_object_new_object();
    json_object_object_add(*result, "results", results);
    json_object_object_add(*result, "trace", trace);
    json_object_object_add(*result, "ledger_id", json_object_new_int64(ledger_id));
    json_object_object_add(*result, "ms", json_object_new_int64(ms));
    free(hits);
    free(own_query);
    thread_ids_free(&matched_entities);
    thread_ids_free(&matched_relationships);
    thread_ids_free(&matched_predicates);
    thread_ids_free(&candidates);
    thread_ids_free(&scoped);
    thread_ids_free(&result_docs);
    return 0;
}

/* MARK: - the graph query */

static struct json_object *entity_json(thread_store *s, const char *owner, const char *id, float score) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, "SELECT name, kind, mention_count FROM entities WHERE id = ?", -1, &stmt, NULL) != SQLITE_OK) return NULL;
    thread_db_bind_text(stmt, 1, id);
    struct json_object *e = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        e = json_object_new_object();
        json_object_object_add(e, "id", json_object_new_string(id));
        json_object_object_add(e, "name", json_object_new_string((const char *)sqlite3_column_text(stmt, 0)));
        json_object_object_add(e, "kind", json_object_new_string((const char *)sqlite3_column_text(stmt, 1)));
        json_object_object_add(e, "score", json_object_new_double(score));
        json_object_object_add(e, "mention_count", json_object_new_int(sqlite3_column_int(stmt, 2)));
    }
    sqlite3_finalize(stmt);
    if (!e) return NULL;
    struct json_object *docs = json_object_new_array();
    if (sqlite3_prepare_v2(s->db, "SELECT ed.document_id FROM entity_documents ed JOIN documents d ON d.id = ed.document_id WHERE ed.entity_id = ? AND d.owner = ? ORDER BY ed.document_id", -1, &stmt, NULL) == SQLITE_OK) {
        thread_db_bind_text(stmt, 1, id);
        thread_db_bind_text(stmt, 2, owner);
        while (sqlite3_step(stmt) == SQLITE_ROW) json_object_array_add(docs, json_object_new_string((const char *)sqlite3_column_text(stmt, 0)));
        sqlite3_finalize(stmt);
    }
    json_object_object_add(e, "document_ids", docs);
    return e;
}

static struct json_object *relationship_json(thread_store *s, const char *owner, const char *id) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, "SELECT subject_id, predicate, object_id, weight FROM relationships WHERE id = ?", -1, &stmt, NULL) != SQLITE_OK) return NULL;
    thread_db_bind_text(stmt, 1, id);
    struct json_object *r = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        r = json_object_new_object();
        json_object_object_add(r, "id", json_object_new_string(id));
        json_object_object_add(r, "subject_id", json_object_new_string((const char *)sqlite3_column_text(stmt, 0)));
        json_object_object_add(r, "predicate", json_object_new_string((const char *)sqlite3_column_text(stmt, 1)));
        json_object_object_add(r, "object_id", json_object_new_string((const char *)sqlite3_column_text(stmt, 2)));
        json_object_object_add(r, "weight", json_object_new_int(sqlite3_column_int(stmt, 3)));
    }
    sqlite3_finalize(stmt);
    if (!r) return NULL;
    struct json_object *docs = json_object_new_array();
    if (sqlite3_prepare_v2(s->db, "SELECT rd.document_id FROM relationship_documents rd JOIN documents d ON d.id = rd.document_id WHERE rd.relationship_id = ? AND d.owner = ? ORDER BY rd.document_id", -1, &stmt, NULL) == SQLITE_OK) {
        thread_db_bind_text(stmt, 1, id);
        thread_db_bind_text(stmt, 2, owner);
        while (sqlite3_step(stmt) == SQLITE_ROW) json_object_array_add(docs, json_object_new_string((const char *)sqlite3_column_text(stmt, 0)));
        sqlite3_finalize(stmt);
    }
    json_object_object_add(r, "document_ids", docs);
    return r;
}

int thread_store_graph_query(thread_store *s, const char *owner, const thread_graph_request *q, struct json_object **result) {
    *result = json_object_new_object();
    struct json_object *entities = json_object_new_array(), *relationships = json_object_new_array(), *documents = json_object_new_array();
    pthread_mutex_lock(&s->lock);
    int64_t entity_count = thread_graph_entity_count(s->db), relationship_count = thread_graph_relationship_count(s->db);
    int limit = q->limit > 0 ? q->limit : 20;
    thread_ids reached = { 0 }, edges = { 0 };
    thread_match *matches = NULL;
    size_t n_matches = 0;
    bool browsing = (!q->entity || !*q->entity) && (!q->query || !*q->query);
    if (entity_count > 0 && browsing) {
        thread_graph_top_entities(s->db, q->kinds, q->n_kinds, limit, &reached);
        thread_graph_edges_among(s->db, &reached, &edges);
    } else if (entity_count > 0) {
        thread_graph_match_entities(s->db, q->entity, q->kinds, q->n_kinds, limit, &matches, &n_matches);
        thread_ids seeds = { 0 };
        for (size_t i = 0; i < n_matches; i++) thread_ids_add(&seeds, matches[i].id);
        thread_ids rel_ids = { 0 };
        if (q->query && *q->query) {
            char message[200];
            const float *vector = thread_store_query_vector(s, q->query, message, sizeof message);
            if (vector) {
                thread_match *rels = NULL;
                size_t n = 0;
                thread_graph_match_relationships(s->db, vector, s->dim, &seeds, limit, &rels, &n);
                for (size_t i = 0; i < n; i++) thread_ids_add(&rel_ids, rels[i].id);
                free(rels);
            }
        }
        thread_graph_endpoints(s->db, &rel_ids, &seeds);
        int hops = q->hops <= 0 ? 1 : q->hops > 3 ? 3 : q->hops;
        thread_graph_neighborhood(s->db, &seeds, hops, &reached, &edges);
        for (size_t i = 0; i < rel_ids.n; i++) thread_ids_add(&edges, rel_ids.v[i]);
        thread_ids_free(&rel_ids);
        thread_ids_free(&seeds);
    }
    for (size_t i = 0; i < reached.n; i++) {
        float score = 0;
        for (size_t m = 0; m < n_matches; m++) if (strcmp(matches[m].id, reached.v[i]) == 0) score = matches[m].score;
        struct json_object *e = entity_json(s, owner, reached.v[i], score);
        if (e) json_object_array_add(entities, e);
    }
    /* scored first, as the Swift sorts by score descending */
    for (size_t i = 0; i < json_object_array_length(entities); i++)
        for (size_t j = i + 1; j < json_object_array_length(entities); j++) {
            double a = 0, b = 0;
            mc_json_double(json_object_array_get_idx(entities, i), "score", &a);
            mc_json_double(json_object_array_get_idx(entities, j), "score", &b);
            if (b > a) {
                struct json_object *x = json_object_get(json_object_array_get_idx(entities, i)), *y = json_object_get(json_object_array_get_idx(entities, j));
                json_object_array_put_idx(entities, i, y);
                json_object_array_put_idx(entities, j, x);
            }
        }
    for (size_t i = 0; i < edges.n; i++) {
        struct json_object *r = relationship_json(s, owner, edges.v[i]);
        if (r) json_object_array_add(relationships, r);
    }
    if (q->include_documents) {
        thread_ids docs = { 0 };
        thread_graph_documents_of_entities(s->db, &reached, &docs);
        for (size_t i = 0; i < docs.n; i++)
            for (size_t j = i + 1; j < docs.n; j++)
                if (strcmp(docs.v[j], docs.v[i]) < 0) { char *t = docs.v[i]; docs.v[i] = docs.v[j]; docs.v[j] = t; }
        for (size_t i = 0; i < docs.n; i++) {
            sqlite3_stmt *stmt = NULL;
            if (sqlite3_prepare_v2(s->db, "SELECT name, owner, family FROM documents WHERE id = ? AND owner = ?", -1, &stmt, NULL) != SQLITE_OK) continue;
            thread_db_bind_text(stmt, 1, docs.v[i]);
            thread_db_bind_text(stmt, 2, owner);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                struct json_object *d = json_object_new_object();
                json_object_object_add(d, "id", json_object_new_string(docs.v[i]));
                json_object_object_add(d, "name", json_object_new_string((const char *)sqlite3_column_text(stmt, 0)));
                json_object_object_add(d, "owner_id", json_object_new_string((const char *)sqlite3_column_text(stmt, 1)));
                json_object_object_add(d, "family", json_object_new_string((const char *)sqlite3_column_text(stmt, 2)));
                json_object_array_add(documents, d);
            }
            sqlite3_finalize(stmt);
        }
        thread_ids_free(&docs);
    }
    pthread_mutex_unlock(&s->lock);
    free(matches);
    thread_ids_free(&reached);
    thread_ids_free(&edges);
    json_object_object_add(*result, "entities", entities);
    json_object_object_add(*result, "relationships", relationships);
    json_object_object_add(*result, "documents", documents);
    json_object_object_add(*result, "entity_count", json_object_new_int64(entity_count));
    json_object_object_add(*result, "relationship_count", json_object_new_int64(relationship_count));
    return 0;
}

int thread_store_graph_mutate(thread_store *s, const char *owner, const char *op, const char *id, const char *name, const char *into,
                              const char *kind, const char *source, struct json_object **result) {
    *result = NULL;
    if (!op || !id) return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = thread_db_begin(s->db);
    thread_mutation m = { { 0 }, { 0 } };
    if (rc == 0) {
        if (strcmp(op, "rename") == 0) rc = name ? thread_graph_rename(s->db, id, name, &m) : -EINVAL;
        else if (strcmp(op, "merge") == 0) rc = into ? thread_graph_merge(s->db, id, into, &m) : -EINVAL;
        else if (strcmp(op, "set_kind") == 0) rc = kind ? thread_graph_set_kind(s->db, id, kind, &m) : -EINVAL;
        else if (strcmp(op, "delete_entity") == 0) rc = thread_graph_delete_entity(s->db, id, &m);
        else if (strcmp(op, "delete_relationship") == 0) rc = thread_graph_delete_relationship(s->db, id);
        else rc = -EINVAL;
    }
    if (rc == 0) {
        struct json_object *detail = json_object_new_object();
        json_object_object_add(detail, "op", json_object_new_string(op));
        json_object_object_add(detail, "id", json_object_new_string(id));
        json_object_object_add(detail, "surviving", json_object_new_string(m.surviving_id));
        add_ids(detail, "affected", &m.affected);
        thread_store_log(s, "mutation", source ? source : "desktop", NULL, NULL, NULL, (int64_t)m.affected.n, 0, detail);
        json_object_put(detail);
        rc = thread_db_commit(s->db);
    } else {
        thread_db_rollback(s->db);
    }
    pthread_mutex_unlock(&s->lock);
    if (rc == 0) {
        *result = json_object_new_object();
        json_object_object_add(*result, "surviving_id", json_object_new_string(m.surviving_id));
        add_ids(*result, "affected_document_ids", &m.affected);
    }
    thread_mutation_free(&m);
    (void)owner;
    return rc;
}

int thread_store_reextract(thread_store *s, const char *owner, const char *document_id, const char *source) {
    if (!thread_id_valid(document_id)) return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = thread_store_owns(s, owner, document_id) ? 0 : -ENOENT;
    if (rc == 0) {
        thread_graph_payload empty = { 0 };
        rc = thread_enrich_enqueue(s, document_id, thread_store_revision(s, document_id), &empty, true);
        if (rc == 0) thread_store_log(s, "extract", source ? source : "desktop", document_id, NULL, NULL, 0, 0, NULL);
    }
    pthread_mutex_unlock(&s->lock);
    if (rc == 0) thread_enrich_signal(s);
    return rc;
}
