/* Retrieval for a turn (Core/Commands/Sewn+Search.swift searchWithThreads): the
 * scope maryd sends, the partitions threadd answers, and the seam production wires
 * to threadd's local socket — /run/thread/local.sock, newline JSON, where sewnd is
 * the trusted caller that may name the owner it searches and writes for. Tests
 * script the seam. threadd embeds the query itself (through sewnd's `embed`). */
#ifndef MARY_SEWN_RETRIEVE_H
#define MARY_SEWN_RETRIEVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SEWN_THREAD_LOCAL_SOCKET "/run/thread/local.sock"
#define SEWN_RETRIEVE_TOP_K 3
#define SEWN_RETRIEVE_TIMEOUT_MS 8000
#define SEWN_SCOPE_LIST_MAX 16

struct json_object;

/* SewnRequest, with the MaryOS lanes: personal | conversation | application | behavioral. */
typedef struct sewn_scope {
    const char *owner_id;
    const char *lanes[SEWN_SCOPE_LIST_MAX];
    size_t n_lanes;
    const char *groups[SEWN_SCOPE_LIST_MAX];
    size_t n_groups;
    const char *entities[SEWN_SCOPE_LIST_MAX];
    size_t n_entities;
    const char *request_id;
    bool aggregate;
    bool global;
} sewn_scope;

/* One of threadd's search results (Sewn.Partition with the record's family and lane). */
typedef struct sewn_partition {
    char *partition_id;
    char *document_id;
    char *owner_id;
    char *text;
    char *name;         /* the document's name: the source title compaction quotes */
    char *family;
    char *lane;
    char *group_id;
    float score;
} sewn_partition;

typedef struct sewn_retrieval {
    sewn_partition *partitions;
    size_t n;
    struct json_object *trace;      /* threadd's graph trace, or NULL */
    int64_t ms;
} sewn_retrieval;

void sewn_retrieval_free(sewn_retrieval *r);
/* Reads `sewn{owner_id, lanes[], groups[], group{id}, entities[], tags[], request_id,
 * aggregate, scope}` from a ChatRequest. Strings are borrowed from `request`. */
void sewn_scope_parse(struct json_object *request, sewn_scope *out);
struct json_object *sewn_scope_json(const sewn_scope *scope);

/* The seam. 0 with *out (possibly empty); -errno when threadd could not be asked. */
typedef int (*sewn_retrieve_fn)(const sewn_scope *scope, const char *query, int top_k, sewn_retrieval *out, char *message, size_t cap, void *user);
/* Writes a document into the owner's group (auto-memory). `tags` may be NULL. */
typedef int (*sewn_deposit_fn)(const char *owner_id, const char *group_id, const char *label, const char *family,
                               const char *const *texts, size_t n, char *document_id, size_t cap, void *user);

/* Over threadd's local socket; `user` is the socket path (const char *), NULL for the default. */
int sewn_thread_retrieve(const sewn_scope *scope, const char *query, int top_k, sewn_retrieval *out, char *message, size_t cap, void *user);
int sewn_thread_deposit(const char *owner_id, const char *group_id, const char *label, const char *family,
                        const char *const *texts, size_t n, char *document_id, size_t cap, void *user);
/* Parses threadd's search.result into *out (tests, and the seam above). */
int sewn_retrieval_parse(struct json_object *result, sewn_retrieval *out);

#endif
