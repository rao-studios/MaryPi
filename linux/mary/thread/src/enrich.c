/* Enrichment: what Thread does after Index answers — embed the partitions, extract
 * entities and relationships when the caller gave none (GraphEnrichment.run), embed
 * the relationship and predicate strings, fold it all into the graph — on one
 * worker thread that talks to sewnd outside the store's lock and writes under it,
 * guarded by the document's revision so a re-deposit in between drops the stale job. */
#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/io.h"
#include "common/json.h"
#include "common/log.h"
#include "foundation/hash.h"
#include "thread/text.h"

int thread_enrich_enqueue(thread_store *s, const char *document_id, int64_t revision, const thread_graph_payload *payload, bool needs_extraction) {
    struct json_object *p = thread_payload_json(payload);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, "INSERT INTO jobs(document_id, revision, step, payload, needs_extraction, attempts, next_ms, created_ms) VALUES(?, ?, 'embed', ?, ?, 0, 0, ?) "
                                  "ON CONFLICT(document_id) DO UPDATE SET revision = excluded.revision, step = 'embed', payload = excluded.payload, "
                                  "needs_extraction = excluded.needs_extraction, attempts = 0, next_ms = 0, last_error = NULL", -1, &stmt, NULL) != SQLITE_OK) {
        json_object_put(p);
        mc_log(MC_LOG_ERROR, "jobs: %s", sqlite3_errmsg(s->db));
        return -EIO;
    }
    thread_db_bind_text(stmt, 1, document_id);
    sqlite3_bind_int64(stmt, 2, revision);
    thread_db_bind_text(stmt, 3, mc_json_compact(p, NULL));
    sqlite3_bind_int(stmt, 4, needs_extraction);
    sqlite3_bind_int64(stmt, 5, mc_wall_ms());
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -EIO;
    sqlite3_finalize(stmt);
    json_object_put(p);
    return rc;
}

typedef struct job {
    int64_t id;
    char document_id[THREAD_ID_MAX + 1];
    int64_t revision;
    char step[16];
    struct json_object *payload;
    bool needs_extraction;
    int attempts;
} job;

/* The next due job, under the lock. false when none. */
static bool next_job(thread_store *s, job *j) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, "SELECT id, document_id, revision, step, payload, needs_extraction, attempts FROM jobs WHERE next_ms <= ? ORDER BY next_ms, id LIMIT 1", -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, mc_wall_ms());
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        memset(j, 0, sizeof *j);
        j->id = sqlite3_column_int64(stmt, 0);
        snprintf(j->document_id, sizeof j->document_id, "%s", (const char *)sqlite3_column_text(stmt, 1));
        j->revision = sqlite3_column_int64(stmt, 2);
        snprintf(j->step, sizeof j->step, "%s", (const char *)sqlite3_column_text(stmt, 3));
        const char *payload = (const char *)sqlite3_column_text(stmt, 4);
        j->payload = payload ? mc_json_parse(payload, strlen(payload)) : NULL;
        j->needs_extraction = sqlite3_column_int(stmt, 5) != 0;
        j->attempts = sqlite3_column_int(stmt, 6);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

static void fail_job(thread_store *s, const job *j, int rc, const char *message) {
    int attempts = j->attempts + 1;
    int64_t delay = rc == -EAGAIN ? THREAD_JOB_RATE_LIMIT_MS : THREAD_JOB_BACKOFF_MS;
    if (rc != -EAGAIN) for (int i = 1; i < attempts && delay < THREAD_JOB_BACKOFF_CAP_MS; i++) delay *= 2;
    if (delay > THREAD_JOB_BACKOFF_CAP_MS) delay = THREAD_JOB_BACKOFF_CAP_MS;
    bool failed = attempts >= THREAD_JOB_ATTEMPTS_MAX;
    if (failed) delay = THREAD_JOB_FAILED_RETRY_MS;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, "UPDATE jobs SET attempts = ?, next_ms = ?, last_error = ? WHERE id = ?", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, attempts);
        sqlite3_bind_int64(stmt, 2, mc_wall_ms() + delay);
        thread_db_bind_text(stmt, 3, message);
        sqlite3_bind_int64(stmt, 4, j->id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    if (failed) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(s->db, "UPDATE documents SET enrich_state = 'failed' WHERE id = ?", -1, &st, NULL) == SQLITE_OK) {
            thread_db_bind_text(st, 1, j->document_id);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    mc_log(rc == -EAGAIN ? MC_LOG_INFO : MC_LOG_WARNING, "enrich %s: %s (attempt %d, again in %llds)", j->document_id, message, attempts, (long long)(delay / 1000));
}

static bool still_current(thread_store *s, const job *j) {
    int64_t current = thread_store_revision(s, j->document_id);
    if (current != j->revision) mc_log(MC_LOG_DEBUG, "enrich: %s is at revision %lld, the job at %lld", j->document_id, (long long)current, (long long)j->revision);
    return current == j->revision;
}

/* Step 1: the partitions without embeddings. */
static int step_embed(thread_store *s, job *j, char *message, size_t cap) {
    pthread_mutex_lock(&s->lock);
    if (!still_current(s, j)) {
        pthread_mutex_unlock(&s->lock);
        return -ESTALE;
    }
    sqlite3_stmt *stmt = NULL;
    int64_t *rowids = NULL;
    char **texts = NULL;
    size_t n = 0;
    if (sqlite3_prepare_v2(s->db, "SELECT rowid, text FROM partitions WHERE document_id = ? AND embedding IS NULL ORDER BY seq", -1, &stmt, NULL) == SQLITE_OK) {
        thread_db_bind_text(stmt, 1, j->document_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            rowids = realloc(rowids, (n + 1) * sizeof *rowids);
            texts = realloc(texts, (n + 1) * sizeof *texts);
            rowids[n] = sqlite3_column_int64(stmt, 0);
            texts[n] = strdup((const char *)sqlite3_column_text(stmt, 1));
            n++;
        }
        sqlite3_finalize(stmt);
    }
    pthread_mutex_unlock(&s->lock);
    int rc = 0;
    float *vectors = NULL;
    if (n) {
        vectors = malloc(n * s->dim * sizeof *vectors);
        rc = vectors ? thread_embedder_embed(s->embedder, (const char *const *)texts, n, vectors, message, cap) : -ENOMEM;
    }
    if (rc == 0) {
        pthread_mutex_lock(&s->lock);
        if (!still_current(s, j)) rc = -ESTALE;
        else {
            thread_db_begin(s->db);
            for (size_t i = 0; rc == 0 && i < n; i++) {
                char pid[MF_NUMERIC_HASH_MAX];
                size_t bytes = s->dim * sizeof(float) + strlen(j->document_id);
                unsigned char *key = malloc(bytes);
                memcpy(key, vectors + i * s->dim, s->dim * sizeof(float));
                memcpy(key + s->dim * sizeof(float), j->document_id, strlen(j->document_id));
                mf_numeric_hash(key, bytes, pid);
                free(key);
                if (sqlite3_prepare_v2(s->db, "UPDATE partitions SET embedding = ?, dim = ?, id = ? WHERE rowid = ?", -1, &stmt, NULL) == SQLITE_OK) {
                    thread_db_bind_floats(stmt, 1, vectors + i * s->dim, s->dim);
                    sqlite3_bind_int(stmt, 2, (int)s->dim);
                    thread_db_bind_text(stmt, 3, pid);
                    sqlite3_bind_int64(stmt, 4, rowids[i]);
                    if (sqlite3_step(stmt) != SQLITE_DONE) rc = -EIO;
                    sqlite3_finalize(stmt);
                }
            }
            if (rc == 0 && sqlite3_prepare_v2(s->db, "UPDATE documents SET enrich_state = 'embedded' WHERE id = ?", -1, &stmt, NULL) == SQLITE_OK) {
                thread_db_bind_text(stmt, 1, j->document_id);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
            if (rc == 0 && sqlite3_prepare_v2(s->db, "UPDATE jobs SET step = ? WHERE id = ?", -1, &stmt, NULL) == SQLITE_OK) {
                thread_db_bind_text(stmt, 1, j->needs_extraction ? "extract" : "graph");
                sqlite3_bind_int64(stmt, 2, j->id);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
            if (rc == 0) {
                thread_db_commit(s->db);
                thread_store_reload_vectors(s, j->document_id);
                if (n) thread_store_log(s, "embed", "sewnd", j->document_id, NULL, NULL, (int64_t)n, 0, NULL);
                snprintf(j->step, sizeof j->step, "%s", j->needs_extraction ? "extract" : "graph");
            } else {
                thread_db_rollback(s->db);
            }
        }
        pthread_mutex_unlock(&s->lock);
    }
    for (size_t i = 0; i < n; i++) free(texts[i]);
    free(texts);
    free(rowids);
    free(vectors);
    return rc;
}

/* Step 2: entities and relationships from the model, when the deposit brought none. */
static int step_extract(thread_store *s, job *j, char *message, size_t cap) {
    pthread_mutex_lock(&s->lock);
    if (!still_current(s, j)) {
        pthread_mutex_unlock(&s->lock);
        return -ESTALE;
    }
    struct json_object *texts = json_object_new_array();
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, "SELECT text FROM partitions WHERE document_id = ? ORDER BY seq", -1, &stmt, NULL) == SQLITE_OK) {
        thread_db_bind_text(stmt, 1, j->document_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) json_object_array_add(texts, json_object_new_string((const char *)sqlite3_column_text(stmt, 0)));
        sqlite3_finalize(stmt);
    }
    char *prompt = thread_policy_system_prompt(&s->policy);
    thread_policy policy;
    size_t plen = 0;
    const char *ptext = mc_json_compact(thread_policy_json(&s->policy), &plen);
    (void)ptext;
    thread_policy_default(&policy);
    {
        struct json_object *pj = thread_policy_json(&s->policy);
        size_t len = 0;
        const char *text = mc_json_compact(pj, &len);
        thread_policy_free(&policy);
        thread_policy_from_json(text, len, &policy);
        json_object_put(pj);
    }
    pthread_mutex_unlock(&s->lock);
    size_t n = json_object_array_length(texts);
    const char **tv = calloc(n ? n : 1, sizeof *tv);
    for (size_t i = 0; i < n; i++) tv[i] = json_object_get_string(json_object_array_get_idx(texts, i));
    char *answer = NULL;
    int rc = s->embedder && s->embedder->extract ? s->embedder->extract(tv, n, prompt, &answer, message, cap, s->embedder->user) : -ENOSYS;
    free(tv);
    free(prompt);
    json_object_put(texts);
    thread_graph_payload extracted = { 0 };
    bool replaced = false;
    if (rc == 0 && answer && thread_graph_payload_parse(answer, strlen(answer), &policy, &extracted) == 0 && (extracted.n_entities || extracted.n_relationships)) replaced = true;
    free(answer);
    thread_policy_free(&policy);
    if (rc == -ENOSYS) rc = 0;          /* no extractor: the keyword entities stand */
    pthread_mutex_lock(&s->lock);
    if (!still_current(s, j)) rc = -ESTALE;
    else {
        if (replaced) {
            json_object_put(j->payload);
            j->payload = thread_payload_json(&extracted);
        }
        if (rc == 0 && sqlite3_prepare_v2(s->db, "UPDATE jobs SET step = 'graph', payload = ?, needs_extraction = 0 WHERE id = ?", -1, &stmt, NULL) == SQLITE_OK) {
            thread_db_bind_text(stmt, 1, mc_json_compact(j->payload, NULL));
            sqlite3_bind_int64(stmt, 2, j->id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            snprintf(j->step, sizeof j->step, "graph");
        }
        if (rc != 0 && rc != -ESTALE) {
            /* extraction failed: keep the keyword entities and carry on (GraphEnrichment) */
            struct json_object *detail = json_object_new_object();
            json_object_object_add(detail, "error", json_object_new_string(message));
            thread_store_log(s, "extract", "sewnd", j->document_id, NULL, NULL, 0, 0, detail);
            json_object_put(detail);
            if (rc != -EAGAIN) {
                if (sqlite3_prepare_v2(s->db, "UPDATE jobs SET step = 'graph', needs_extraction = 0 WHERE id = ?", -1, &stmt, NULL) == SQLITE_OK) {
                    sqlite3_bind_int64(stmt, 1, j->id);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
                snprintf(j->step, sizeof j->step, "graph");
                rc = 0;
            }
        } else if (rc == 0 && replaced) {
            thread_store_log(s, "extract", "sewnd", j->document_id, NULL, NULL, (int64_t)extracted.n_entities, 0, NULL);
        }
    }
    pthread_mutex_unlock(&s->lock);
    thread_payload_free(&extracted);
    return rc;
}

/* Step 3: the policy's edges, the relationship and predicate embeddings, and the upsert. */
static int step_graph(thread_store *s, job *j, char *message, size_t cap) {
    thread_graph_payload payload = { 0 };
    if (j->payload) thread_payload_from_json(j->payload, &payload);
    pthread_mutex_lock(&s->lock);
    if (!still_current(s, j)) {
        pthread_mutex_unlock(&s->lock);
        thread_payload_free(&payload);
        return -ESTALE;
    }
    thread_graph_apply_policy(s->db, &payload, &s->policy);
    /* which relationship and predicate strings still need an embedding */
    char **strings = NULL;
    size_t n_strings = 0;
    int *rel_index = calloc(payload.n_relationships ? payload.n_relationships : 1, sizeof *rel_index);
    int *pred_index = calloc(payload.n_relationships ? payload.n_relationships : 1, sizeof *pred_index);
    for (size_t i = 0; i < payload.n_relationships; i++) {
        rel_index[i] = pred_index[i] = -1;
        const thread_relation_in *r = &payload.relationships[i];
        char *sn = thread_normalize_name(r->subject), *on = thread_normalize_name(r->object), *pred = thread_lower_trim(r->predicate);
        const char *skind = NULL, *okind = NULL, *sname = NULL, *oname = NULL;
        for (size_t e = 0; e < payload.n_entities; e++) {
            char *en = thread_normalize_name(payload.entities[e].name);
            if (en && sn && !sname && strcmp(en, sn) == 0) { sname = payload.entities[e].name; skind = payload.entities[e].kind; }
            if (en && on && !oname && strcmp(en, on) == 0) { oname = payload.entities[e].name; okind = payload.entities[e].kind; }
            free(en);
        }
        char *sk_db = NULL, *ok_db = NULL, *sname_db = NULL, *oname_db = NULL;
        if (!sname && sn) {
            sname_db = thread_db_text(s->db, "SELECT name FROM entities WHERE name_norm = ? LIMIT 1", sn, NULL);
            sk_db = thread_db_text(s->db, "SELECT kind FROM entities WHERE name_norm = ? LIMIT 1", sn, NULL);
            sname = sname_db;
            skind = sk_db;
        }
        if (!oname && on) {
            oname_db = thread_db_text(s->db, "SELECT name FROM entities WHERE name_norm = ? LIMIT 1", on, NULL);
            ok_db = thread_db_text(s->db, "SELECT kind FROM entities WHERE name_norm = ? LIMIT 1", on, NULL);
            oname = oname_db;
            okind = ok_db;
        }
        if (sname && oname && pred && *pred) {
            char sid[THREAD_GRAPH_ID_MAX], oid[THREAD_GRAPH_ID_MAX], rid[THREAD_GRAPH_ID_MAX], pid[THREAD_GRAPH_ID_MAX];
            thread_entity_id(skind, sname, sid);
            thread_entity_id(okind, oname, oid);
            if (strcmp(sid, oid) != 0) {
                thread_relationship_id(sid, pred, oid, rid);
                thread_predicate_id(pred, pid);
                if (!r->embedding && thread_db_int(s->db, "SELECT COUNT(*) FROM relationships WHERE id = ? AND embedding IS NOT NULL", rid, NULL) == 0) {
                    char text[1024];
                    thread_relationship_embed_string(skind, sname, pred, okind, oname, text, sizeof text);
                    strings = realloc(strings, (n_strings + 1) * sizeof *strings);
                    strings[n_strings] = strdup(text);
                    rel_index[i] = (int)n_strings++;
                }
                if (!r->predicate_embedding && thread_db_int(s->db, "SELECT COUNT(*) FROM predicates WHERE id = ? AND embedding IS NOT NULL", pid, NULL) == 0) {
                    /* one string per distinct predicate */
                    int existing = -1;
                    for (size_t k = 0; k < i; k++) {
                        char *pk = thread_lower_trim(payload.relationships[k].predicate);
                        if (pk && strcmp(pk, pred) == 0 && pred_index[k] >= 0) existing = pred_index[k];
                        free(pk);
                    }
                    if (existing >= 0) pred_index[i] = existing;
                    else {
                        char text[256];
                        thread_predicate_embed_string(pred, text, sizeof text);
                        strings = realloc(strings, (n_strings + 1) * sizeof *strings);
                        strings[n_strings] = strdup(text);
                        pred_index[i] = (int)n_strings++;
                    }
                }
            }
        }
        free(sn);
        free(on);
        free(pred);
        free(sk_db);
        free(ok_db);
        free(sname_db);
        free(oname_db);
    }
    pthread_mutex_unlock(&s->lock);
    int rc = 0;
    float *vectors = NULL;
    if (n_strings && s->embedder && s->embedder->embed) {
        vectors = malloc(n_strings * s->dim * sizeof *vectors);
        rc = thread_embedder_embed(s->embedder, (const char *const *)strings, n_strings, vectors, message, cap);
        if (rc == 0) {
            for (size_t i = 0; i < payload.n_relationships; i++) {
                if (rel_index[i] >= 0) {
                    payload.relationships[i].embedding = malloc(s->dim * sizeof(float));
                    memcpy(payload.relationships[i].embedding, vectors + (size_t)rel_index[i] * s->dim, s->dim * sizeof(float));
                }
                if (pred_index[i] >= 0) {
                    payload.relationships[i].predicate_embedding = malloc(s->dim * sizeof(float));
                    memcpy(payload.relationships[i].predicate_embedding, vectors + (size_t)pred_index[i] * s->dim, s->dim * sizeof(float));
                }
            }
        } else if (rc != -EAGAIN) {
            mc_log(MC_LOG_WARNING, "enrich %s: relationship embedding failed: %s; the graph stays navigable by name", j->document_id, message);
            rc = 0;
        }
    }
    free(vectors);
    for (size_t i = 0; i < n_strings; i++) free(strings[i]);
    free(strings);
    free(rel_index);
    free(pred_index);
    if (rc == 0) {
        pthread_mutex_lock(&s->lock);
        if (!still_current(s, j)) rc = -ESTALE;
        else {
            rc = thread_db_begin(s->db);
            if (rc == 0) rc = thread_graph_detach(s->db, j->document_id);
            if (rc == 0) rc = thread_graph_upsert(s->db, j->document_id, &payload, s->dim);
            sqlite3_stmt *stmt = NULL;
            if (rc == 0 && sqlite3_prepare_v2(s->db, "UPDATE documents SET enrich_state = 'done' WHERE id = ?", -1, &stmt, NULL) == SQLITE_OK) {
                thread_db_bind_text(stmt, 1, j->document_id);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
            if (rc == 0 && sqlite3_prepare_v2(s->db, "DELETE FROM jobs WHERE id = ?", -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, j->id);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
            if (rc == 0) {
                struct json_object *detail = json_object_new_object();
                json_object_object_add(detail, "entities", json_object_new_int64((int64_t)payload.n_entities));
                json_object_object_add(detail, "relationships", json_object_new_int64((int64_t)payload.n_relationships));
                thread_store_log(s, "extract", "threadd", j->document_id, NULL, NULL, (int64_t)payload.n_entities, 0, detail);
                json_object_put(detail);
                rc = thread_db_commit(s->db);
                snprintf(j->step, sizeof j->step, "done");
            } else {
                thread_db_rollback(s->db);
            }
        }
        pthread_mutex_unlock(&s->lock);
    }
    thread_payload_free(&payload);
    return rc;
}

static int run_job(thread_store *s, job *j) {
    char message[256] = "";
    int rc = 0;
    while (rc == 0 && strcmp(j->step, "done") != 0) {
        if (strcmp(j->step, "embed") == 0) rc = step_embed(s, j, message, sizeof message);
        else if (strcmp(j->step, "extract") == 0) rc = step_extract(s, j, message, sizeof message);
        else if (strcmp(j->step, "graph") == 0) rc = step_graph(s, j, message, sizeof message);
        else rc = -EINVAL;
    }
    if (rc == -ESTALE) {
        /* The document changed underneath. Its re-deposit re-queued this row at the new
         * revision, so only a row still carrying the stale revision is dropped. */
        pthread_mutex_lock(&s->lock);
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(s->db, "DELETE FROM jobs WHERE id = ? AND revision = ?", -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, j->id);
            sqlite3_bind_int64(stmt, 2, j->revision);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        pthread_mutex_unlock(&s->lock);
        return 0;
    }
    if (rc) {
        pthread_mutex_lock(&s->lock);
        fail_job(s, j, rc, *message ? message : strerror(-rc));
        pthread_mutex_unlock(&s->lock);
    }
    return rc;
}

int thread_store_enrich_drain(thread_store *s) {
    if (!s->embedder) return 0;
    int ran = 0;
    for (int guard = 0; guard < 10000; guard++) {
        job j;
        pthread_mutex_lock(&s->lock);
        bool found = next_job(s, &j);
        pthread_mutex_unlock(&s->lock);
        if (!found) break;
        int rc = run_job(s, &j);
        mc_log(MC_LOG_DEBUG, "enrich: job %lld for %s ended at step %s with %d", (long long)j.id, j.document_id, j.step, rc);
        if (j.payload) json_object_put(j.payload);
        ran++;
        /* a job that failed is not due again now; stop when every due job has been tried once */
        pthread_mutex_lock(&s->lock);
        int64_t due = thread_db_int(s->db, "SELECT COUNT(*) FROM jobs WHERE next_ms <= ?", NULL, NULL);
        pthread_mutex_unlock(&s->lock);
        (void)due;
    }
    return ran;
}

static void *worker(void *arg) {
    thread_store *s = arg;
    for (;;) {
        pthread_mutex_lock(&s->jobs_lock);
        if (s->stopping) {
            pthread_mutex_unlock(&s->jobs_lock);
            break;
        }
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += THREAD_WORKER_POLL_MS / 1000;
        pthread_cond_timedwait(&s->jobs_cond, &s->jobs_lock, &until);
        bool stopping = s->stopping;
        pthread_mutex_unlock(&s->jobs_lock);
        if (stopping) break;
        thread_store_enrich_drain(s);
    }
    return NULL;
}

int thread_enrich_start(thread_store *s) {
    if (!s->embedder || s->worker_running) return 0;
    s->stopping = false;
    if (pthread_create(&s->worker, NULL, worker, s) != 0) return -errno;
    s->worker_running = true;
    thread_enrich_signal(s);
    return 0;
}

void thread_enrich_signal(thread_store *s) {
    pthread_mutex_lock(&s->jobs_lock);
    pthread_cond_signal(&s->jobs_cond);
    pthread_mutex_unlock(&s->jobs_lock);
}

void thread_enrich_stop(thread_store *s) {
    if (!s->worker_running) return;
    pthread_mutex_lock(&s->jobs_lock);
    s->stopping = true;
    pthread_cond_broadcast(&s->jobs_cond);
    pthread_mutex_unlock(&s->jobs_lock);
    pthread_join(s->worker, NULL);
    s->worker_running = false;
}
