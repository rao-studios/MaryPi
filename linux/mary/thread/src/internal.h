/* What store.c, search.c, enrich.c, parity.c and proto.c share and nobody else sees. */
#ifndef MARY_THREAD_INTERNAL_H
#define MARY_THREAD_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>

#include "thread/db.h"
#include "thread/embedder.h"
#include "thread/graph.h"
#include "thread/store.h"
#include "thread/vectors.h"

#define THREAD_QUERY_CACHE 64
#define THREAD_JOB_ATTEMPTS_MAX 20
#define THREAD_JOB_BACKOFF_MS 5000
#define THREAD_JOB_BACKOFF_CAP_MS (60 * 60 * 1000)
#define THREAD_JOB_RATE_LIMIT_MS 60000
#define THREAD_JOB_FAILED_RETRY_MS (6 * 60 * 60 * 1000)
#define THREAD_WORKER_POLL_MS 60000

typedef struct query_entry {
    char *text;
    float *vector;
    unsigned used;
} query_entry;

struct thread_store {
    char dir[512];
    char db_path[600];
    char node_id[37];
    sqlite3 *db;
    pthread_mutex_t lock;
    thread_vectors *vectors;
    const thread_embedder *embedder;
    size_t dim;
    thread_policy policy;
    /* the query embedding cache (QueryEmbeddingCache) */
    query_entry queries[THREAD_QUERY_CACHE];
    unsigned query_clock;
    /* enrichment */
    bool worker_wanted;
    bool worker_running;
    bool stopping;
    pthread_t worker;
    pthread_mutex_t jobs_lock;
    pthread_cond_t jobs_cond;
};

/* enrich.c */
int thread_enrich_start(thread_store *s);
void thread_enrich_stop(thread_store *s);
void thread_enrich_signal(thread_store *s);
/* Queues (or re-queues) a document's enrichment; under the store lock. */
int thread_enrich_enqueue(thread_store *s, const char *document_id, int64_t revision, const thread_graph_payload *payload, bool needs_extraction);

/* search.c: the query vector for `text`, cached; under the store lock. NULL when there is
 * no embedder or it failed (message says why). The result is owned by the cache. */
const float *thread_store_query_vector(thread_store *s, const char *text, char *message, size_t cap);
/* The owner's documents that pass the group and lane filters. */
int thread_store_candidates(thread_store *s, const char *owner, const char *const *group_ids, size_t n_groups,
                            const char *const *lanes, size_t n_lanes, thread_ids *out);

/* store.c */
int64_t thread_store_revision(thread_store *s, const char *document_id);
/* Reloads one document's vectors from the database into the arena. */
int thread_store_reload_vectors(thread_store *s, const char *document_id);
/* Removes one document under the lock: rows, vectors, graph. */
int thread_store_remove_document(thread_store *s, const char *document_id);
void thread_store_log(thread_store *s, const char *kind, const char *source, const char *document_id, const char *group_id,
                      const char *request_id, int64_t count, int64_t ms, struct json_object *detail);
/* A document's partitions in order: [{rowid, seq, text, embedded, id}] */
struct json_object *thread_store_partitions_json(thread_store *s, const char *document_id);
bool thread_store_owns(thread_store *s, const char *owner, const char *document_id);
/* One document with its texts, under the lock; NULL when it is not the owner's. */
struct json_object *thread_store_document_json(thread_store *s, const char *owner, const char *id);

/* "file-<fnv(canonical(owner)|path)>": the id every record of a file gets (parity.c). */
void thread_file_document_id(const char *owner, const char *path, char out[THREAD_ID_MAX + 1]);

#endif
