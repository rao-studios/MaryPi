#include <math.h>
#include <stdlib.h>

#include "mary_test.h"
#include "thread/db.h"
#include "thread/vectors.h"

/* 16-dimensional toy vectors: one float per sub-vector, so the distance is the L1 distance. */
static void unit(float *v, int axis, float scale) {
    for (int i = 0; i < 16; i++) v[i] = 0;
    v[axis] = scale;
}

MARY_TEST(the_distance_is_the_sum_of_sub_vector_distances) {
    float a[16], b[16];
    unit(a, 0, 3);
    unit(b, 0, 0);
    MARY_ASSERT_NEAR(thread_vectors_distance(a, b, 16), 3, 1e-6);
    unit(b, 1, 4);
    MARY_ASSERT_NEAR(thread_vectors_distance(a, b, 16), 7, 1e-6);      /* 3 + 4, not 5: per sub-vector */
    float c[32], d[32];
    for (int i = 0; i < 32; i++) { c[i] = 1; d[i] = 0; }
    MARY_ASSERT_NEAR(thread_vectors_distance(c, d, 32), 16 * sqrtf(2), 1e-4);
}

MARY_TEST(blocks_are_replaced_dropped_and_searched_per_document) {
    thread_vectors *v = thread_vectors_new(16);
    float vecs[3 * 16];
    unit(vecs, 0, 1);
    unit(vecs + 16, 0, 5);
    unit(vecs + 32, 0, 9);
    int64_t rows[3] = { 11, 12, 13 };
    MARY_ASSERT_EQ(thread_vectors_put(v, "doc", rows, vecs, 3), 0);
    MARY_ASSERT_EQ(thread_vectors_count(v), 3);
    float q[16];
    unit(q, 0, 4);
    thread_hit hits[3];
    int n = thread_vectors_search_doc(v, q, "doc", 3, 8.0f, hits);
    MARY_ASSERT_EQ(n, 3);                                  /* 1, 3, 5 are all under 8 */
    MARY_ASSERT_EQ(hits[0].rowid, 12);
    MARY_ASSERT_NEAR(hits[0].score, 1, 1e-6);
    MARY_ASSERT_EQ(hits[1].rowid, 11);
    MARY_ASSERT_EQ(hits[2].rowid, 13);
    n = thread_vectors_search_doc(v, q, "doc", 3, 2.0f, hits);
    MARY_ASSERT_EQ(n, 1);                                  /* only the one under the threshold */
    unit(q, 0, 100);
    n = thread_vectors_search_doc(v, q, "doc", 2, 8.0f, hits);
    MARY_ASSERT_EQ(n, 2);                                  /* none pass: all k stay */
    MARY_ASSERT_EQ(hits[0].rowid, 13);
    thread_hit best;
    MARY_ASSERT_EQ(thread_vectors_best(v, q, "doc", &best), 1);
    MARY_ASSERT_EQ(best.rowid, 13);
    MARY_ASSERT_EQ(thread_vectors_best(v, q, "nope", &best), 0);
    /* replacing shrinks; dropping removes */
    MARY_ASSERT_EQ(thread_vectors_put(v, "doc", rows, vecs, 1), 0);
    MARY_ASSERT_EQ(thread_vectors_count(v), 1);
    MARY_ASSERT_EQ(thread_vectors_rename(v, "doc", "moved"), 0);
    MARY_ASSERT_EQ(thread_vectors_search_doc(v, q, "moved", 3, 8.0f, hits), 1);
    thread_vectors_drop(v, "moved");
    MARY_ASSERT_EQ(thread_vectors_count(v), 0);
    MARY_ASSERT_EQ(thread_vectors_documents(v), 0);
    thread_vectors_free(v);
}

MARY_TEST(a_scan_over_many_documents_matches_one_at_a_time) {
    thread_vectors *v = thread_vectors_new(16);
    char names[40][8];
    const char *docs[40];
    for (int d = 0; d < 40; d++) {
        snprintf(names[d], sizeof names[d], "d%d", d);
        docs[d] = names[d];
        float vecs[2 * 16];
        unit(vecs, 0, (float)d);
        unit(vecs + 16, 1, (float)d);
        int64_t rows[2] = { d * 2, d * 2 + 1 };
        thread_vectors_put(v, names[d], rows, vecs, 2);
    }
    float q[16];
    unit(q, 0, 10);
    thread_hit *out = calloc(40 * 2, sizeof *out);
    int counts[40];
    MARY_ASSERT_EQ(thread_vectors_scan(v, q, docs, 40, 2, 8.0f, out, counts), 0);
    for (int d = 0; d < 40; d++) {
        thread_hit alone[2];
        int n = thread_vectors_search_doc(v, q, docs[d], 2, 8.0f, alone);
        MARY_ASSERT_EQ(counts[d], n);
        for (int i = 0; i < n; i++) MARY_ASSERT_EQ(out[d * 2 + i].rowid, alone[i].rowid);
    }
    MARY_ASSERT_EQ(counts[10], 1);                         /* d10's first vector is at distance 0 */
    free(out);
    thread_vectors_free(v);
}

MARY_TEST(the_arena_loads_what_the_database_holds) {
    sqlite3 *db = NULL;
    MARY_ASSERT_EQ(thread_db_open(":memory:", &db), 0);
    thread_db_exec(db, "INSERT INTO documents(id, owner, created_at) VALUES('a', 'mary', 0), ('b', 'mary', 0);");
    float e[16];
    unit(e, 0, 2);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db, "INSERT INTO partitions(document_id, seq, text, embedding, dim) VALUES(?, ?, 'x', ?, 16)", -1, &stmt, NULL);
    for (int i = 0; i < 3; i++) {
        sqlite3_reset(stmt);
        thread_db_bind_text(stmt, 1, i < 2 ? "a" : "b");
        sqlite3_bind_int(stmt, 2, i);
        thread_db_bind_floats(stmt, 3, e, 16);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    thread_db_exec(db, "INSERT INTO partitions(document_id, seq, text) VALUES('b', 9, 'not embedded yet')");
    thread_vectors *v = thread_vectors_new(16);
    MARY_ASSERT_EQ(thread_vectors_load(v, db), 0);
    MARY_ASSERT_EQ(thread_vectors_count(v), 3);
    MARY_ASSERT_EQ(thread_vectors_documents(v), 2);
    MARY_ASSERT_EQ(thread_vectors_bytes(v), 3 * 16 * sizeof(float));
    thread_vectors_free(v);
    thread_db_close(db);
}

int main(void) {
    MARY_RUN(the_distance_is_the_sum_of_sub_vector_distances);
    MARY_RUN(blocks_are_replaced_dropped_and_searched_per_document);
    MARY_RUN(a_scan_over_many_documents_matches_one_at_a_time);
    MARY_RUN(the_arena_loads_what_the_database_holds);
    MARY_TEST_MAIN_END();
}
