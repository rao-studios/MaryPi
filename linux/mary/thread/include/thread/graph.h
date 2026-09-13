/* The knowledge graph (Thread's Sources/Database/Graph): content-addressed entities
 * — (kind, normalized name) merges the same thing across documents — weighted
 * relationships, predicates, and the provenance that ties every node and edge back
 * to the documents it came from. Every function works on the store's SQLite handle;
 * the caller holds the store's lock, and the writers run inside its transaction. */
#ifndef MARY_THREAD_GRAPH_H
#define MARY_THREAD_GRAPH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sqlite3.h>

#include "foundation/hash.h"

#define THREAD_GRAPH_ID_MAX MF_NUMERIC_HASH_MAX
#define THREAD_RELATIONSHIP_MATCH_THRESHOLD 0.15f
#define THREAD_PREDICATE_MATCH_THRESHOLD 0.15f
#define THREAD_PREDICATE_SCORE_WEIGHT 0.8f
#define THREAD_ENTITY_MATCH_LIMIT 8
#define THREAD_RELATIONSHIP_MATCH_LIMIT 12
#define THREAD_AUTO_PREDICATE_PREFIX "auto:"

struct json_object;

/* A set of ids that keeps insertion order. */
typedef struct thread_ids {
    char **v;
    size_t n, cap;
} thread_ids;
int thread_ids_add(thread_ids *ids, const char *id);      /* 1 added, 0 already there, -ENOMEM */
bool thread_ids_has(const thread_ids *ids, const char *id);
void thread_ids_free(thread_ids *ids);

/* GraphPayload: what a document contributes (Database.GraphPayload). */
typedef struct thread_entity_in {
    char *name;
    char *kind;
} thread_entity_in;
typedef struct thread_relation_in {
    char *subject, *predicate, *object;
    float *embedding;               /* dim floats, or NULL */
    float *predicate_embedding;
} thread_relation_in;
typedef struct thread_graph_payload {
    thread_entity_in *entities;
    size_t n_entities;
    thread_relation_in *relationships;
    size_t n_relationships;
} thread_graph_payload;
int thread_payload_add_entity(thread_graph_payload *p, const char *name, const char *kind);
int thread_payload_add_relation(thread_graph_payload *p, const char *subject, const char *predicate, const char *object);
void thread_payload_free(thread_graph_payload *p);
/* {"entities":[{name,kind}],"relationships":[{subject,predicate,object}]} without embeddings. */
struct json_object *thread_payload_json(const thread_graph_payload *p);
int thread_payload_from_json(struct json_object *o, thread_graph_payload *out);

/* ExtractionPolicy (Graph/ExtractionPolicy.swift). */
typedef struct thread_policy_kind {
    char name[32];
    char description[160];
} thread_policy_kind;
typedef struct thread_policy {
    thread_policy_kind kinds[16];
    size_t n_kinds;
    int max_entities;               /* 12 */
    int max_relationships;          /* 15 */
    bool co_mention;                /* off */
    char co_mention_predicate[64];  /* "appears with" */
    bool skip_explicitly_linked;
    int hub_degree_cap;             /* 24; 0 = none */
    struct json_object *aliases;    /* alias → canonical, or NULL */
    char *prompt_template;          /* NULL: the built-in */
} thread_policy;
void thread_policy_default(thread_policy *p);
void thread_policy_free(thread_policy *p);
int thread_policy_from_json(const char *json, size_t len, thread_policy *out);
struct json_object *thread_policy_json(const thread_policy *p);
/* effectiveSystemPrompt. Heap. */
char *thread_policy_system_prompt(const thread_policy *p);
/* The canonical predicate: trimmed, lowercased, through the alias map. Heap. */
char *thread_policy_normalize_predicate(const thread_policy *p, const char *predicate);
/* GraphExtractionParser.parse: the model's answer (bare JSON or wrapped in prose) into a
 * payload, kinds coerced to the ontology, caps applied, relationships kept only between
 * emitted entities. -EINVAL when no JSON object is there. */
int thread_graph_payload_parse(const char *response, size_t len, const thread_policy *policy, thread_graph_payload *out);
/* applyPolicyEdges: predicate aliases, then co-mention edges (when enabled) between every
 * pair of the payload's entities not explicitly linked and under the hub cap. */
int thread_graph_apply_policy(sqlite3 *db, thread_graph_payload *p, const thread_policy *policy);

/* Identity (GraphStore.entityID / relationshipID / predicateID). */
void thread_entity_id(const char *kind, const char *name, char out[THREAD_GRAPH_ID_MAX]);
void thread_relationship_id(const char *subject_id, const char *predicate, const char *object_id, char out[THREAD_GRAPH_ID_MAX]);
void thread_predicate_id(const char *predicate, char out[THREAD_GRAPH_ID_MAX]);
/* "Relationship predicate: <p>" and "<kind>: <Subject> <predicate> <kind>: <Object>". */
void thread_predicate_embed_string(const char *predicate, char *out, size_t cap);
void thread_relationship_embed_string(const char *subject_kind, const char *subject, const char *predicate,
                                      const char *object_kind, const char *object, char *out, size_t cap);

/* Writers. */
int thread_graph_upsert(sqlite3 *db, const char *document_id, const thread_graph_payload *p, size_t dim);
int thread_graph_detach(sqlite3 *db, const char *document_id);

typedef struct thread_mutation {
    char surviving_id[THREAD_GRAPH_ID_MAX];    /* "" for a delete */
    thread_ids affected;                       /* documents whose entity lists changed */
} thread_mutation;
void thread_mutation_free(thread_mutation *m);
int thread_graph_rename(sqlite3 *db, const char *id, const char *new_name, thread_mutation *out);
int thread_graph_set_kind(sqlite3 *db, const char *id, const char *kind, thread_mutation *out);
int thread_graph_merge(sqlite3 *db, const char *from, const char *into, thread_mutation *out);
int thread_graph_delete_entity(sqlite3 *db, const char *id, thread_mutation *out);
int thread_graph_delete_relationship(sqlite3 *db, const char *id);

/* Readers. */
typedef struct thread_match {
    char id[THREAD_GRAPH_ID_MAX];
    float score;
    char predicate_id[THREAD_GRAPH_ID_MAX];    /* relationships only */
    int weight;
} thread_match;
/* Entities whose name shares a token with the query, kind-filtered, score 1.0, at most
 * `limit` by mention count then id. *out is heap. */
int thread_graph_match_entities(sqlite3 *db, const char *name_query, const char *const *kinds, size_t nkinds, int limit,
                                thread_match **out, size_t *n);
/* Relationships by embedding: dot with the edge, else 0.8 × the predicate's, floored at
 * the threshold when an endpoint is a seed; sorted by score then weight. */
int thread_graph_match_relationships(sqlite3 *db, const float *q, size_t dim, const thread_ids *seeds, int limit,
                                     thread_match **out, size_t *n);
int thread_graph_neighborhood(sqlite3 *db, const thread_ids *seeds, int hops, thread_ids *entities, thread_ids *edges);
int thread_graph_documents_of_entities(sqlite3 *db, const thread_ids *entities, thread_ids *docs);
int thread_graph_documents_of_relationships(sqlite3 *db, const thread_ids *edges, thread_ids *docs);
int thread_graph_endpoints(sqlite3 *db, const thread_ids *edges, thread_ids *entities);
/* Browse mode: the top entities by (mention count, name) descending, kind-filtered. */
int thread_graph_top_entities(sqlite3 *db, const char *const *kinds, size_t nkinds, int limit, thread_ids *out);
/* Every edge with both endpoints in `entities`. */
int thread_graph_edges_among(sqlite3 *db, const thread_ids *entities, thread_ids *edges);
/* The sum of the weights of `edges` whose provenance holds `document_id`. */
int thread_graph_edge_weight_for(sqlite3 *db, const char *document_id, const thread_ids *edges);
int64_t thread_graph_entity_count(sqlite3 *db);
int64_t thread_graph_relationship_count(sqlite3 *db);
int thread_graph_entity_degree(sqlite3 *db, const char *id);
bool thread_graph_has_entity(sqlite3 *db, const char *id);

/* The dot product of two vectors. */
float thread_dot(const float *a, const float *b, size_t dim);

#endif
