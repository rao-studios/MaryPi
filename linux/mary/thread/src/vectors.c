#include "thread/vectors.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "thread/db.h"

typedef struct block {
    char *document_id;
    int64_t *rowids;
    float *vecs;
    size_t n;
} block;

struct thread_vectors {
    size_t dim;
    block *blocks;
    size_t n, cap;
    size_t vectors;
};

thread_vectors *thread_vectors_new(size_t dim) {
    thread_vectors *v = calloc(1, sizeof *v);
    if (v) v->dim = dim ? dim : THREAD_EMBEDDING_DIM;
    return v;
}

static void block_free(block *b) {
    free(b->document_id);
    free(b->rowids);
    free(b->vecs);
}

void thread_vectors_free(thread_vectors *v) {
    if (!v) return;
    for (size_t i = 0; i < v->n; i++) block_free(&v->blocks[i]);
    free(v->blocks);
    free(v);
}

static block *find(const thread_vectors *v, const char *document_id) {
    for (size_t i = 0; i < v->n; i++) if (strcmp(v->blocks[i].document_id, document_id) == 0) return &v->blocks[i];
    return NULL;
}

int thread_vectors_put(thread_vectors *v, const char *document_id, const int64_t *rowids, const float *vecs, size_t n) {
    block *b = find(v, document_id);
    if (!b) {
        if (n == 0) return 0;
        if (v->n == v->cap) {
            size_t cap = v->cap ? v->cap * 2 : 64;
            block *grown = realloc(v->blocks, cap * sizeof *grown);
            if (!grown) return -ENOMEM;
            v->blocks = grown;
            v->cap = cap;
        }
        b = &v->blocks[v->n];
        memset(b, 0, sizeof *b);
        b->document_id = strdup(document_id);
        if (!b->document_id) return -ENOMEM;
        v->n++;
    }
    v->vectors -= b->n;
    free(b->rowids);
    free(b->vecs);
    b->rowids = NULL;
    b->vecs = NULL;
    b->n = 0;
    if (n) {
        b->rowids = malloc(n * sizeof *b->rowids);
        b->vecs = malloc(n * v->dim * sizeof *b->vecs);
        if (!b->rowids || !b->vecs) {
            free(b->rowids);
            free(b->vecs);
            b->rowids = NULL;
            b->vecs = NULL;
            return -ENOMEM;
        }
        memcpy(b->rowids, rowids, n * sizeof *b->rowids);
        memcpy(b->vecs, vecs, n * v->dim * sizeof *b->vecs);
        b->n = n;
    }
    v->vectors += n;
    return 0;
}

void thread_vectors_drop(thread_vectors *v, const char *document_id) {
    block *b = find(v, document_id);
    if (!b) return;
    v->vectors -= b->n;
    block_free(b);
    size_t i = (size_t)(b - v->blocks);
    memmove(&v->blocks[i], &v->blocks[i + 1], (v->n - i - 1) * sizeof *b);
    v->n--;
}

int thread_vectors_rename(thread_vectors *v, const char *from, const char *to) {
    block *b = find(v, from);
    if (!b) return 0;
    char *name = strdup(to);
    if (!name) return -ENOMEM;
    free(b->document_id);
    b->document_id = name;
    return 0;
}

int thread_vectors_load(thread_vectors *v, sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT document_id, rowid, embedding FROM partitions WHERE embedding IS NOT NULL ORDER BY document_id, seq", -1, &stmt, NULL) != SQLITE_OK) {
        mc_log(MC_LOG_ERROR, "vectors: load: %s", sqlite3_errmsg(db));
        return -EIO;
    }
    char current[256] = "";
    int64_t *rowids = NULL;
    float *vecs = NULL;
    size_t n = 0, cap = 0;
    int rc = 0;
    while (rc == 0 && sqlite3_step(stmt) == SQLITE_ROW) {
        const char *doc = (const char *)sqlite3_column_text(stmt, 0);
        if (strcmp(doc, current) != 0) {
            if (n) rc = thread_vectors_put(v, current, rowids, vecs, n);
            n = 0;
            snprintf(current, sizeof current, "%s", doc);
        }
        float *e = thread_db_column_floats(stmt, 2, v->dim);
        if (!e) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            int64_t *r = realloc(rowids, cap * sizeof *r);
            float *f = realloc(vecs, cap * v->dim * sizeof *f);
            if (!r || !f) {
                free(r ? r : rowids);
                free(f ? f : vecs);
                free(e);
                sqlite3_finalize(stmt);
                return -ENOMEM;
            }
            rowids = r;
            vecs = f;
        }
        rowids[n] = sqlite3_column_int64(stmt, 1);
        memcpy(vecs + n * v->dim, e, v->dim * sizeof *vecs);
        free(e);
        n++;
    }
    if (rc == 0 && n) rc = thread_vectors_put(v, current, rowids, vecs, n);
    free(rowids);
    free(vecs);
    sqlite3_finalize(stmt);
    return rc;
}

size_t thread_vectors_count(const thread_vectors *v) { return v->vectors; }
size_t thread_vectors_documents(const thread_vectors *v) { return v->n; }
size_t thread_vectors_bytes(const thread_vectors *v) { return v->vectors * v->dim * sizeof(float); }
size_t thread_vectors_dim(const thread_vectors *v) { return v->dim; }

float thread_vectors_distance(const float *a, const float *b, size_t dim) {
    size_t sub = dim / THREAD_SUBVECTORS;
    if (!sub) sub = dim;
    float total = 0;
    for (size_t s = 0; s < dim; s += sub) {
        float acc = 0;
        size_t end = s + sub <= dim ? s + sub : dim;
        for (size_t i = s; i < end; i++) {
            float d = a[i] - b[i];
            acc += d * d;
        }
        total += sqrtf(acc);
    }
    return total;
}

/* The k nearest of a block, ascending, by bounded insertion. */
static int top_k(const thread_vectors *v, const block *b, const float *q, int k, thread_hit *out) {
    int count = 0;
    for (size_t i = 0; i < b->n; i++) {
        float d = thread_vectors_distance(q, b->vecs + i * v->dim, v->dim);
        if (count == k && d >= out[count - 1].score) continue;
        int lo = 0, hi = count;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (out[mid].score < d) lo = mid + 1;
            else hi = mid;
        }
        if (count < k) count++;
        memmove(&out[lo + 1], &out[lo], (size_t)(count - 1 - lo) * sizeof *out);
        out[lo].rowid = b->rowids[i];
        out[lo].score = d;
    }
    return count;
}

int thread_vectors_search_doc(const thread_vectors *v, const float *q, const char *document_id, int k, float threshold, thread_hit *out) {
    block *b = find(v, document_id);
    if (!b || k <= 0) return 0;
    int count = top_k(v, b, q, k, out);
    int kept = 0;
    for (int i = 0; i < count; i++) if (out[i].score < threshold) kept++;
    if (kept == 0) return count;            /* none pass: every candidate stays */
    return kept;                            /* ascending, so the passing ones lead */
}

int thread_vectors_best(const thread_vectors *v, const float *q, const char *document_id, thread_hit *out) {
    block *b = find(v, document_id);
    if (!b || !b->n) return 0;
    return top_k(v, b, q, 1, out);
}

struct scan_job {
    const thread_vectors *v;
    const float *q;
    const char *const *docs;
    size_t from, to;
    int k;
    float threshold;
    thread_hit *out;
    int *counts;
};

static void *scan_run(void *arg) {
    struct scan_job *j = arg;
    for (size_t i = j->from; i < j->to; i++)
        j->counts[i] = thread_vectors_search_doc(j->v, j->q, j->docs[i], j->k, j->threshold, j->out + i * (size_t)j->k);
    return NULL;
}

int thread_vectors_scan(const thread_vectors *v, const float *q, const char *const *docs, size_t ndocs, int k, float threshold,
                        thread_hit *out, int *counts) {
    enum { THREADS = 4 };
    if (ndocs < 8) {
        struct scan_job j = { v, q, docs, 0, ndocs, k, threshold, out, counts };
        scan_run(&j);
        return 0;
    }
    struct scan_job jobs[THREADS];
    pthread_t threads[THREADS];
    size_t per = (ndocs + THREADS - 1) / THREADS;
    int started = 0;
    for (int t = 0; t < THREADS; t++) {
        size_t from = (size_t)t * per, to = from + per > ndocs ? ndocs : from + per;
        jobs[t] = (struct scan_job){ v, q, docs, from, to, k, threshold, out, counts };
        if (from >= to) break;
        if (pthread_create(&threads[t], NULL, scan_run, &jobs[t]) != 0) {
            scan_run(&jobs[t]);
            continue;
        }
        started = t + 1;
    }
    for (int t = 0; t < started; t++) pthread_join(threads[t], NULL);
    return 0;
}
