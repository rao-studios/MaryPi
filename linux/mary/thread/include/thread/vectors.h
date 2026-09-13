/* The vectors in memory: every embedded partition's float32 embedding, kept
 * contiguous per document so a search scans one document's block at a time
 * (Thread's PartitionIndex.search, minus the product quantization: at a personal
 * scale the exact distance is faster to build and strictly better, and the PQ
 * stage can sit behind this same interface later). The distance is the sum over
 * 16 sub-vectors of the Euclidean distance — the form ADC computes exactly — with
 * Thread's default threshold of 8.0 (16 × 0.5). */
#ifndef MARY_THREAD_VECTORS_H
#define MARY_THREAD_VECTORS_H

#include <stddef.h>
#include <stdint.h>

#include <sqlite3.h>

#define THREAD_SUBVECTORS 16
#define THREAD_DISTANCE_THRESHOLD 8.0f
#define THREAD_SEARCH_K 3
#define THREAD_EXPANSION_PENALTY 1.1f

typedef struct thread_vectors thread_vectors;

typedef struct thread_hit {
    int64_t rowid;          /* the partition row */
    float score;            /* the distance: lower is closer */
} thread_hit;

thread_vectors *thread_vectors_new(size_t dim);
void thread_vectors_free(thread_vectors *v);
/* Every partition with an embedding, from the database. */
int thread_vectors_load(thread_vectors *v, sqlite3 *db);
/* Replaces a document's block. `vecs` holds n × dim floats. */
int thread_vectors_put(thread_vectors *v, const char *document_id, const int64_t *rowids, const float *vecs, size_t n);
void thread_vectors_drop(thread_vectors *v, const char *document_id);
/* Renames a document's block (file.move). */
int thread_vectors_rename(thread_vectors *v, const char *from, const char *to);
size_t thread_vectors_count(const thread_vectors *v);
size_t thread_vectors_documents(const thread_vectors *v);
size_t thread_vectors_bytes(const thread_vectors *v);
size_t thread_vectors_dim(const thread_vectors *v);

/* Σ over the 16 sub-vectors of ‖a_i − b_i‖. */
float thread_vectors_distance(const float *a, const float *b, size_t dim);
/* The k nearest partitions of one document, ascending; those under `threshold`, or
 * all k when none pass (PartitionIndex.search). Returns the count written to out. */
int thread_vectors_search_doc(const thread_vectors *v, const float *q, const char *document_id, int k, float threshold, thread_hit *out);
/* The nearest partition of one document, with no threshold. 1, or 0 when the
 * document has no vectors. */
int thread_vectors_best(const thread_vectors *v, const float *q, const char *document_id, thread_hit *out);
/* search_doc over many documents at once, on a few threads: out holds ndocs × k
 * slots and counts[i] says how many the i-th document filled. */
int thread_vectors_scan(const thread_vectors *v, const float *q, const char *const *docs, size_t ndocs, int k, float threshold,
                        thread_hit *out, int *counts);

#endif
