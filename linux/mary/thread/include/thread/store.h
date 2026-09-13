/* Thread's memory on MaryOS: the hard drive's own record of everything on it. One
 * SQLite file, thread.db, in the state directory (thread/db.h has the tables), the
 * embeddings of every chunk in memory beside it (thread/vectors.h), the knowledge
 * graph (thread/graph.h), a ledger of what was done (thread/ledger.h), the record
 * families (thread/families.h), and the file parity rows indexd keeps in step.
 *
 * `owner` is always the caller as the kernel reports it — or the owner sewnd names,
 * because sewnd is trusted the way Sewn is the Mac's mothership — and nobody may
 * replace or read another owner's documents. Calls are serialized by one lock;
 * embedding and extraction happen on a worker thread that holds the lock only for
 * its short writes (thread/embedder.h says where the vectors come from). The
 * gRPC-facing twins of these calls, over thread.v1's messages, are thread/proto.h. */
#ifndef MARY_THREAD_STORE_H
#define MARY_THREAD_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sqlite3.h>

#include "thread/embedder.h"
#include "thread/graph.h"

#define THREAD_STATE_DIR "/var/lib/thread"
#define THREAD_ID_MAX 200
#define THREAD_SEARCH_MAX 1000
#define THREAD_SEWN_USER "sewn"

struct json_object;
typedef struct thread_store thread_store;

typedef struct thread_store_options {
    const char *dir;                    /* the state directory; thread.db lives inside */
    const thread_embedder *embedder;    /* NULL: nothing is embedded or extracted */
    bool worker;                        /* run the enrichment thread */
    size_t dim;                         /* 0: 1024 */
} thread_store_options;

/* Opens the database (importing the legacy JSON files on first open, then moving
 * them to legacy/), loads the vectors, starts the worker when asked. NULL with *error set. */
thread_store *thread_store_open_with(const thread_store_options *options, int *error);
/* thread_store_open_with(dir, no embedder, no worker). */
thread_store *thread_store_open(const char *dir, int *error);
void thread_store_close(thread_store *store);
const char *thread_store_node_id(const thread_store *store);
/* The handle, for tests and for readers that hold the lock themselves. */
sqlite3 *thread_store_db(thread_store *store);
void thread_store_lock(thread_store *store);
void thread_store_unlock(thread_store *store);

/* An id: 1…200 bytes of UTF-8 with no control bytes, whitespace or "/". Ids are no
 * longer file names, so `|` and `:` are welcome (mary-routing-<intent>|<skill>|<epoch>). */
bool thread_id_valid(const char *id);

/* One document, the way ThreadDirectClient.deposit(DepositItem) and indexd's file
 * records arrive. Either `texts` (the partitions as given, the gRPC Index shape) or
 * `text` (chunked when `chunk` is set, else one partition). */
typedef struct thread_deposit {
    const char *document_id;        /* NULL: Thread's computeHash of the texts */
    const char *group_id;           /* NULL: no group */
    const char *group_label;        /* relabels the group when given */
    const char *family;             /* NULL: classified from the id, group and metadata */
    const char *const *texts;
    size_t n_texts;
    const char *text;
    bool chunk;
    const char *name;
    const char *media_type;         /* NULL: "text" */
    const char *metadata;           /* bytes, verbatim; JSON on MaryOS */
    size_t metadata_len;
    const char *const *tags;        /* NULL: TagGenerator's */
    size_t n_tags;
    const thread_graph_payload *graph;  /* entities and relationships, or NULL: tags as concepts, then extraction */
    /* a file record (indexd) */
    const char *file_path;
    const char *file_kind;
    int64_t file_size;
    int64_t file_mtime_ms;
    const char *file_hash;
    bool file_text_indexed;
    /* the ledger */
    const char *source;             /* indexd | maryd | sewnd | threadctl | grpc */
    const char *request_id;
} thread_deposit;

/* Writes the document, replacing an earlier one of this owner with the same id (its
 * created_at kept, its revision bumped; unchanged texts keep their embeddings), joins
 * the group (created on first use), detaches and re-upserts its graph contribution,
 * queues its enrichment, and logs the deposit. 0 with the id and partition count;
 * -EINVAL (id, group or no text), -EPERM (someone else's), -errno. */
int thread_store_deposit(thread_store *store, const char *owner, const thread_deposit *d, char document_id[THREAD_ID_MAX + 1], size_t *partitions);
/* The same from JSON: {document_id?, group?, label?, family?, texts[] | text, chunk?, name?,
 * media_type?, metadata (an object or a string), tags[], entities[{name,kind}],
 * relationships[{subject,predicate,object}], file{path, kind, size, mtime_ms, hash,
 * text_indexed}, source?, request_id?} → {document_id, partitions, family}. */
int thread_store_deposit_json(thread_store *store, const char *owner, struct json_object *request, struct json_object **reply);

/* Removes the owner's documents among `ids` (every one of the owner's when n is 0):
 * partitions, vectors, graph provenance, file rows. */
int thread_store_remove(thread_store *store, const char *owner, const char *const *ids, size_t n, const char *source, size_t *removed);
/* ThreadUpdate.UpdateGroup / UpdateDocument. */
int thread_store_update_group(thread_store *store, const char *owner, const char *group_id, const char *access, const char *label,
                              const char *description, const char *const *tags, size_t n_tags, bool update_metadata, bool *updated);
int thread_store_update_document(thread_store *store, const char *owner, const char *document_id, const char *access, const char *group_id, bool *updated);

/* Reading, as JSON (proto.h turns these into thread.v1 messages):
 *   library    {groups:[{id, label, owner_id, family, access, description, tags,
 *                        documents:[{id, name, created_at, family}]}], has_more}
 *   documents  {documents:[{id, name, owner_id, group_id, group_label, family, created_at,
 *                           media_type, texts:[…], metadata, tags, enrich_state}]}
 *   export     {documents:[…], has_more} */
struct json_object *thread_store_library_json(thread_store *store, const char *owner, int limit, const char *after_id,
                                              const char *const *document_ids, size_t n_ids);
struct json_object *thread_store_documents_json(thread_store *store, const char *owner, const char *const *ids, size_t n);
struct json_object *thread_store_export_json(thread_store *store, const char *owner, const char *const *group_ids, size_t n_groups,
                                             const char *prefix, const char *after_id, int limit);
/* {node_id, documents, partitions, embedded, groups, owners, entities, relationships,
 *  predicates, files, ledger_rows, jobs_pending, jobs_failed, db_bytes, wal_bytes,
 *  vectors:{count, documents, bytes, dim}, embedding_model, user_version} */
struct json_object *thread_store_stats_json(thread_store *store);
/* The families, each with its live count and last_written_ms. */
struct json_object *thread_store_schemas_json(thread_store *store);

/* Search (Thread's PartitionTable.search, ThreadQueryServiceImpl.search). */
typedef struct thread_search_request {
    const char *query_text;         /* embedded here (via the embedder) unless query_embedding is given */
    const float *query_embedding;
    size_t dim;
    const char *const *entities;    /* relationship hints, embedded as "Relationship predicates: …" */
    size_t n_entities;
    const char *const *group_ids;   /* narrows to these groups */
    size_t n_groups;
    const char *const *lanes;       /* narrows to these lanes' families (thread/families.h) */
    size_t n_lanes;
    bool aggregate;
    bool global;
    int top_k;                      /* 0: THREAD_SEARCH_MAX */
    const char *request_id;
    const char *source;
} thread_search_request;
/* {results:[{partition_id, document_id, owner_id, text, score, family, lane, group_id, name}],
 *  trace: {matched_entity_ids, matched_relationship_ids, matched_predicate_ids,
 *          expansion_edge_ids, expanded_document_count} | null, ledger_id, ms}
 * -ENOSYS when there is no embedder and no query embedding. */
int thread_store_search(thread_store *store, const char *owner, const thread_search_request *request, struct json_object **result);

/* ThreadGraph.Query. */
typedef struct thread_graph_request {
    const char *entity;             /* seed by name, or NULL */
    const char *query;              /* free text, embedded here, or NULL */
    const char *const *kinds;
    size_t n_kinds;
    int hops;                       /* 0 → 1, clamped to 3 */
    int limit;                      /* 0 → 20 */
    bool include_documents;
} thread_graph_request;
/* {entities:[{id, name, kind, score, mention_count, document_ids}], relationships:[{id, subject_id,
 *  predicate, object_id, weight, document_ids}], documents:[{id, name, owner_id, family}],
 *  entity_count, relationship_count} */
int thread_store_graph_query(thread_store *store, const char *owner, const thread_graph_request *request, struct json_object **result);
/* op: rename{id, name} | merge{id, into} | set_kind{id, kind} | delete_entity{id} | delete_relationship{id}.
 * → {surviving_id, affected_document_ids}. -ENOENT, -EINVAL. */
int thread_store_graph_mutate(thread_store *store, const char *owner, const char *op, const char *id, const char *name, const char *into,
                              const char *kind, const char *source, struct json_object **result);
/* Queues a document for extraction again. */
int thread_store_reextract(thread_store *store, const char *owner, const char *document_id, const char *source);
struct json_object *thread_store_policy_json(thread_store *store);
int thread_store_policy_set(thread_store *store, struct json_object *policy);

/* Files and parity (indexd). entries: [{path, size, mtime_ms, hash}]; the first page opens a
 * run (*run_id 0 → set), `last` closes it and removes the rows the run never saw.
 * → {run_id, seen, recorded, missing, stale, orphaned, entries:[{path, status}]} — the
 * entries listed are the ones that are not `recorded`. */
int thread_store_parity_report(thread_store *store, const char *owner, int64_t *run_id, struct json_object *entries, bool last, struct json_object **report);
/* The last finished run: the totals and its entries, or {} when none. */
struct json_object *thread_store_parity_last(thread_store *store, const char *owner);
/* One file's record: {document, partitions:[{seq, text, embedded, id}], file:{…}, entities:[…],
 * relationships:[…], ledger:[…], enrich_state}. -ENOENT. */
int thread_store_file_record(thread_store *store, const char *owner, const char *path_or_id, struct json_object **out);
int thread_store_file_move(thread_store *store, const char *owner, const char *from, const char *to, const char *source);
int thread_store_file_remove(thread_store *store, const char *owner, const char *path, const char *source, bool *removed);

/* Runs every due enrichment job now, on this thread (tests, threadctl --once). The
 * number of jobs run, or a negative errno. */
int thread_store_enrich_drain(thread_store *store);
int thread_store_backup(thread_store *store, const char *path);
int thread_store_checkpoint(thread_store *store);

#endif
