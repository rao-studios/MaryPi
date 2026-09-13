/* Files and parity: the rows indexd keeps in step with the drive, and the report
 * that proves it — every file on the drive has its record, and no record outlives
 * its file. The graph never keeps a node whose only provenance was a file that is
 * gone: removing the document detaches it, and detach deletes what nothing holds. */
#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/io.h"
#include "common/json.h"
#include "common/log.h"
#include "foundation/hash.h"
#include "thread/ledger.h"

static char *document_of_path(thread_store *s, const char *owner, const char *path) {
    return thread_db_text(s->db, "SELECT id FROM files WHERE owner = ? AND path = ?", owner, path);
}

int thread_store_file_record(thread_store *s, const char *owner, const char *path_or_id, struct json_object **out) {
    *out = NULL;
    if (!path_or_id || !*path_or_id) return -EINVAL;
    pthread_mutex_lock(&s->lock);
    char *id = document_of_path(s, owner, path_or_id);
    if (!id && thread_id_valid(path_or_id) && thread_store_owns(s, owner, path_or_id)) id = strdup(path_or_id);
    if (!id) {
        pthread_mutex_unlock(&s->lock);
        return -ENOENT;
    }
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "document", thread_store_document_json(s, owner, id));
    json_object_object_add(o, "partitions", thread_store_partitions_json(s, id));
    sqlite3_stmt *stmt = NULL;
    struct json_object *file = NULL;
    if (sqlite3_prepare_v2(s->db, "SELECT path, kind, size, mtime_ms, content_hash, text_indexed, seen_ms FROM files WHERE id = ?", -1, &stmt, NULL) == SQLITE_OK) {
        thread_db_bind_text(stmt, 1, id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            file = json_object_new_object();
            json_object_object_add(file, "path", json_object_new_string((const char *)sqlite3_column_text(stmt, 0)));
            json_object_object_add(file, "kind", json_object_new_string((const char *)sqlite3_column_text(stmt, 1)));
            json_object_object_add(file, "size", json_object_new_int64(sqlite3_column_int64(stmt, 2)));
            json_object_object_add(file, "mtime_ms", json_object_new_int64(sqlite3_column_int64(stmt, 3)));
            json_object_object_add(file, "hash", json_object_new_string((const char *)sqlite3_column_text(stmt, 4)));
            json_object_object_add(file, "text_indexed", json_object_new_boolean(sqlite3_column_int(stmt, 5)));
            json_object_object_add(file, "seen_ms", json_object_new_int64(sqlite3_column_int64(stmt, 6)));
        }
        sqlite3_finalize(stmt);
    }
    json_object_object_add(o, "file", file);
    struct json_object *entities = json_object_new_array(), *relationships = json_object_new_array();
    thread_ids eids = { 0 };
    if (sqlite3_prepare_v2(s->db, "SELECT e.id, e.name, e.kind, e.mention_count FROM entity_documents ed JOIN entities e ON e.id = ed.entity_id WHERE ed.document_id = ? ORDER BY ed.position", -1, &stmt, NULL) == SQLITE_OK) {
        thread_db_bind_text(stmt, 1, id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            struct json_object *e = json_object_new_object();
            json_object_object_add(e, "id", json_object_new_string((const char *)sqlite3_column_text(stmt, 0)));
            json_object_object_add(e, "name", json_object_new_string((const char *)sqlite3_column_text(stmt, 1)));
            json_object_object_add(e, "kind", json_object_new_string((const char *)sqlite3_column_text(stmt, 2)));
            json_object_object_add(e, "mention_count", json_object_new_int(sqlite3_column_int(stmt, 3)));
            json_object_array_add(entities, e);
            thread_ids_add(&eids, (const char *)sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    for (size_t i = 0; i < eids.n; i++) {
        if (sqlite3_prepare_v2(s->db, "SELECT r.id, r.subject_id, s.name, r.predicate, r.object_id, o.name, r.weight FROM relationships r "
                                      "JOIN entities s ON s.id = r.subject_id JOIN entities o ON o.id = r.object_id WHERE r.subject_id = ?1 OR r.object_id = ?1", -1, &stmt, NULL) != SQLITE_OK) continue;
        thread_db_bind_text(stmt, 1, eids.v[i]);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *rid = (const char *)sqlite3_column_text(stmt, 0);
            bool seen = false;
            for (size_t k = 0; k < json_object_array_length(relationships); k++)
                if (strcmp(mc_json_string(json_object_array_get_idx(relationships, k), "id"), rid) == 0) seen = true;
            if (seen) continue;
            struct json_object *r = json_object_new_object();
            json_object_object_add(r, "id", json_object_new_string(rid));
            json_object_object_add(r, "subject_id", json_object_new_string((const char *)sqlite3_column_text(stmt, 1)));
            json_object_object_add(r, "subject", json_object_new_string((const char *)sqlite3_column_text(stmt, 2)));
            json_object_object_add(r, "predicate", json_object_new_string((const char *)sqlite3_column_text(stmt, 3)));
            json_object_object_add(r, "object_id", json_object_new_string((const char *)sqlite3_column_text(stmt, 4)));
            json_object_object_add(r, "object", json_object_new_string((const char *)sqlite3_column_text(stmt, 5)));
            json_object_object_add(r, "weight", json_object_new_int(sqlite3_column_int(stmt, 6)));
            json_object_array_add(relationships, r);
        }
        sqlite3_finalize(stmt);
    }
    thread_ids_free(&eids);
    json_object_object_add(o, "entities", entities);
    json_object_object_add(o, "relationships", relationships);
    struct json_object *rows = NULL;
    int more = 0;
    thread_ledger_filter f = { .document_id = id, .limit = 20 };
    thread_ledger_page(s->db, &f, &rows, &more);
    json_object_object_add(o, "ledger", rows ? rows : json_object_new_array());
    char *state = thread_db_text(s->db, "SELECT enrich_state FROM documents WHERE id = ?", id, NULL);
    json_object_object_add(o, "enrich_state", json_object_new_string(state ? state : "pending"));
    free(state);
    pthread_mutex_unlock(&s->lock);
    free(id);
    *out = o;
    return 0;
}

int thread_store_file_remove(thread_store *s, const char *owner, const char *path, const char *source, bool *removed) {
    *removed = false;
    pthread_mutex_lock(&s->lock);
    char *id = document_of_path(s, owner, path);
    int rc = 0;
    if (id) {
        rc = thread_db_begin(s->db);
        if (rc == 0) rc = thread_store_remove_document(s, id);
        if (rc == 0) {
            struct json_object *detail = json_object_new_object();
            json_object_object_add(detail, "path", json_object_new_string(path));
            thread_store_log(s, "remove", source ? source : "indexd", id, NULL, NULL, 1, 0, detail);
            json_object_put(detail);
            rc = thread_db_commit(s->db);
            *removed = rc == 0;
        } else {
            thread_db_rollback(s->db);
        }
    }
    free(id);
    pthread_mutex_unlock(&s->lock);
    return rc;
}

int thread_store_file_move(thread_store *s, const char *owner, const char *from, const char *to, const char *source) {
    if (!from || !to || !*to) return -EINVAL;
    pthread_mutex_lock(&s->lock);
    char *old_id = document_of_path(s, owner, from);
    if (!old_id) {
        pthread_mutex_unlock(&s->lock);
        return -ENOENT;
    }
    char key[4096], hex[17];
    char *canon_owner = mf_canonical(owner);
    snprintf(key, sizeof key, "%s|%s", canon_owner ? canon_owner : owner, to);
    free(canon_owner);
    mf_fnv1a64_hex(key, hex);
    char new_id[THREAD_ID_MAX + 1];
    snprintf(new_id, sizeof new_id, "file-%s", hex);
    int rc = thread_db_begin(s->db);
    /* a record already at the destination is replaced */
    if (rc == 0 && strcmp(new_id, old_id) != 0 && thread_db_int(s->db, "SELECT COUNT(*) FROM documents WHERE id = ?", new_id, NULL) > 0)
        rc = thread_store_remove_document(s, new_id);
    sqlite3_stmt *stmt = NULL;
    if (rc == 0 && sqlite3_prepare_v2(s->db, "UPDATE documents SET id = ?, updated_ms = ? WHERE id = ?", -1, &stmt, NULL) == SQLITE_OK) {
        thread_db_bind_text(stmt, 1, new_id);
        sqlite3_bind_int64(stmt, 2, mc_wall_ms());
        thread_db_bind_text(stmt, 3, old_id);
        rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -EIO;   /* cascades to partitions, files, provenance, jobs, stats */
        sqlite3_finalize(stmt);
    }
    if (rc == 0 && sqlite3_prepare_v2(s->db, "UPDATE files SET path = ?, seen_ms = ? WHERE id = ?", -1, &stmt, NULL) == SQLITE_OK) {
        thread_db_bind_text(stmt, 1, to);
        sqlite3_bind_int64(stmt, 2, mc_wall_ms());
        thread_db_bind_text(stmt, 3, new_id);
        rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -EIO;
        sqlite3_finalize(stmt);
    }
    /* partition ids hash the document id: recompute them */
    if (rc == 0 && sqlite3_prepare_v2(s->db, "SELECT rowid, embedding FROM partitions WHERE document_id = ? AND embedding IS NOT NULL", -1, &stmt, NULL) == SQLITE_OK) {
        thread_db_bind_text(stmt, 1, new_id);
        while (rc == 0 && sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t rowid = sqlite3_column_int64(stmt, 0);
            size_t bytes = (size_t)sqlite3_column_bytes(stmt, 1);
            unsigned char *key2 = malloc(bytes + strlen(new_id));
            memcpy(key2, sqlite3_column_blob(stmt, 1), bytes);
            memcpy(key2 + bytes, new_id, strlen(new_id));
            char pid[MF_NUMERIC_HASH_MAX];
            mf_numeric_hash(key2, bytes + strlen(new_id), pid);
            free(key2);
            sqlite3_stmt *up = NULL;
            if (sqlite3_prepare_v2(s->db, "UPDATE partitions SET id = ? WHERE rowid = ?", -1, &up, NULL) == SQLITE_OK) {
                thread_db_bind_text(up, 1, pid);
                sqlite3_bind_int64(up, 2, rowid);
                if (sqlite3_step(up) != SQLITE_DONE) rc = -EIO;
                sqlite3_finalize(up);
            }
        }
        sqlite3_finalize(stmt);
    }
    if (rc == 0) {
        thread_vectors_rename(s->vectors, old_id, new_id);
        struct json_object *detail = json_object_new_object();
        json_object_object_add(detail, "moved_from", json_object_new_string(from));
        json_object_object_add(detail, "path", json_object_new_string(to));
        json_object_object_add(detail, "was", json_object_new_string(old_id));
        thread_store_log(s, "index", source ? source : "indexd", new_id, NULL, NULL, 1, 0, detail);
        json_object_put(detail);
        rc = thread_db_commit(s->db);
    } else {
        thread_db_rollback(s->db);
    }
    free(old_id);
    pthread_mutex_unlock(&s->lock);
    return rc;
}

int thread_store_parity_report(thread_store *s, const char *owner, int64_t *run_id, struct json_object *entries, bool last, struct json_object **report) {
    *report = NULL;
    pthread_mutex_lock(&s->lock);
    int rc = thread_db_begin(s->db);
    sqlite3_stmt *stmt = NULL;
    int64_t started = 0;
    if (rc == 0 && (!run_id || *run_id <= 0)) {
        started = mc_wall_ms();
        if (sqlite3_prepare_v2(s->db, "INSERT INTO parity_runs(owner, started_ms) VALUES(?, ?)", -1, &stmt, NULL) == SQLITE_OK) {
            thread_db_bind_text(stmt, 1, owner);
            sqlite3_bind_int64(stmt, 2, started);
            rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -EIO;
            sqlite3_finalize(stmt);
            if (rc == 0 && run_id) *run_id = sqlite3_last_insert_rowid(s->db);
        }
    }
    int64_t id = run_id ? *run_id : sqlite3_last_insert_rowid(s->db);
    if (rc == 0) {
        char idstr[32];
        snprintf(idstr, sizeof idstr, "%lld", (long long)id);
        started = thread_db_int(s->db, "SELECT started_ms FROM parity_runs WHERE id = ? AND owner = ?", idstr, owner);
        if (started <= 0) rc = -ENOENT;
    }
    int64_t seen = 0, recorded = 0, missing = 0, stale = 0, orphaned = 0;
    struct json_object *listed = json_object_new_array();
    for (size_t i = 0; rc == 0 && entries && i < json_object_array_length(entries); i++) {
        struct json_object *e = json_object_array_get_idx(entries, i);
        const char *path = mc_json_string(e, "path"), *hash = mc_json_string(e, "hash");
        int64_t size = 0, mtime = 0;
        mc_json_int64(e, "size", &size);
        mc_json_int64(e, "mtime_ms", &mtime);
        if (!path) continue;
        seen++;
        const char *status = "missing";
        if (sqlite3_prepare_v2(s->db, "SELECT id, content_hash, size, mtime_ms FROM files WHERE owner = ? AND path = ?", -1, &stmt, NULL) == SQLITE_OK) {
            thread_db_bind_text(stmt, 1, owner);
            thread_db_bind_text(stmt, 2, path);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *have_hash = (const char *)sqlite3_column_text(stmt, 1);
                bool same = hash && have_hash && strcmp(hash, have_hash) == 0 && sqlite3_column_int64(stmt, 2) == size && sqlite3_column_int64(stmt, 3) == mtime;
                status = same ? "recorded" : "stale";
                const char *fid = (const char *)sqlite3_column_text(stmt, 0);
                sqlite3_stmt *touch = NULL;
                if (sqlite3_prepare_v2(s->db, "UPDATE files SET seen_ms = ? WHERE id = ?", -1, &touch, NULL) == SQLITE_OK) {
                    sqlite3_bind_int64(touch, 1, started);
                    thread_db_bind_text(touch, 2, fid);
                    sqlite3_step(touch);
                    sqlite3_finalize(touch);
                }
            }
            sqlite3_finalize(stmt);
        }
        if (strcmp(status, "recorded") == 0) recorded++;
        else {
            if (strcmp(status, "missing") == 0) missing++;
            else stale++;
            struct json_object *row = json_object_new_object();
            json_object_object_add(row, "path", json_object_new_string(path));
            json_object_object_add(row, "status", json_object_new_string(status));
            json_object_array_add(listed, row);
        }
    }
    if (rc == 0 && last) {
        thread_ids orphans = { 0 };
        if (sqlite3_prepare_v2(s->db, "SELECT id, path FROM files WHERE owner = ? AND seen_ms < ?", -1, &stmt, NULL) == SQLITE_OK) {
            thread_db_bind_text(stmt, 1, owner);
            sqlite3_bind_int64(stmt, 2, started);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                thread_ids_add(&orphans, (const char *)sqlite3_column_text(stmt, 0));
                struct json_object *row = json_object_new_object();
                json_object_object_add(row, "path", json_object_new_string((const char *)sqlite3_column_text(stmt, 1)));
                json_object_object_add(row, "status", json_object_new_string("orphaned"));
                json_object_array_add(listed, row);
            }
            sqlite3_finalize(stmt);
        }
        for (size_t i = 0; rc == 0 && i < orphans.n; i++) {
            rc = thread_store_remove_document(s, orphans.v[i]);
            struct json_object *detail = json_object_new_object();
            json_object_object_add(detail, "reason", json_object_new_string("orphaned"));
            thread_store_log(s, "remove", "indexd", orphans.v[i], NULL, NULL, 1, 0, detail);
            json_object_put(detail);
            orphaned++;
        }
        thread_ids_free(&orphans);
    }
    if (rc == 0 && sqlite3_prepare_v2(s->db, "UPDATE parity_runs SET seen = seen + ?, recorded = recorded + ?, missing = missing + ?, stale = stale + ?, orphaned = orphaned + ?, "
                                             "entries = ?, finished_ms = ? WHERE id = ?", -1, &stmt, NULL) == SQLITE_OK) {
        char idstr[32];
        snprintf(idstr, sizeof idstr, "%lld", (long long)id);
        char *previous = thread_db_text(s->db, "SELECT entries FROM parity_runs WHERE id = ?", idstr, NULL);
        struct json_object *all = previous ? mc_json_parse(previous, strlen(previous)) : NULL;
        if (!all) all = json_object_new_array();
        for (size_t i = 0; i < json_object_array_length(listed); i++) json_object_array_add(all, json_object_get(json_object_array_get_idx(listed, i)));
        free(previous);
        sqlite3_bind_int64(stmt, 1, seen);
        sqlite3_bind_int64(stmt, 2, recorded);
        sqlite3_bind_int64(stmt, 3, missing);
        sqlite3_bind_int64(stmt, 4, stale);
        sqlite3_bind_int64(stmt, 5, orphaned);
        thread_db_bind_text(stmt, 6, mc_json_compact(all, NULL));
        if (last) sqlite3_bind_int64(stmt, 7, mc_wall_ms());
        else sqlite3_bind_null(stmt, 7);
        sqlite3_bind_int64(stmt, 8, id);
        rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -EIO;
        sqlite3_finalize(stmt);
        json_object_put(all);
    }
    if (rc == 0 && last) {
        struct json_object *detail = json_object_new_object();
        char idstr[32];
        snprintf(idstr, sizeof idstr, "%lld", (long long)id);
        json_object_object_add(detail, "seen", json_object_new_int64(thread_db_int(s->db, "SELECT seen FROM parity_runs WHERE id = ?", idstr, NULL)));
        json_object_object_add(detail, "recorded", json_object_new_int64(thread_db_int(s->db, "SELECT recorded FROM parity_runs WHERE id = ?", idstr, NULL)));
        json_object_object_add(detail, "missing", json_object_new_int64(thread_db_int(s->db, "SELECT missing FROM parity_runs WHERE id = ?", idstr, NULL)));
        json_object_object_add(detail, "stale", json_object_new_int64(thread_db_int(s->db, "SELECT stale FROM parity_runs WHERE id = ?", idstr, NULL)));
        json_object_object_add(detail, "orphaned", json_object_new_int64(orphaned));
        thread_store_log(s, "reconcile", "indexd", NULL, NULL, NULL, seen, mc_wall_ms() - started, detail);
        json_object_put(detail);
    }
    if (rc == 0) rc = thread_db_commit(s->db);
    else thread_db_rollback(s->db);
    if (rc == 0) {
        char idstr[32];
        snprintf(idstr, sizeof idstr, "%lld", (long long)id);
        *report = json_object_new_object();
        json_object_object_add(*report, "run_id", json_object_new_int64(id));
        json_object_object_add(*report, "seen", json_object_new_int64(thread_db_int(s->db, "SELECT seen FROM parity_runs WHERE id = ?", idstr, NULL)));
        json_object_object_add(*report, "recorded", json_object_new_int64(thread_db_int(s->db, "SELECT recorded FROM parity_runs WHERE id = ?", idstr, NULL)));
        json_object_object_add(*report, "missing", json_object_new_int64(thread_db_int(s->db, "SELECT missing FROM parity_runs WHERE id = ?", idstr, NULL)));
        json_object_object_add(*report, "stale", json_object_new_int64(thread_db_int(s->db, "SELECT stale FROM parity_runs WHERE id = ?", idstr, NULL)));
        json_object_object_add(*report, "orphaned", json_object_new_int64(thread_db_int(s->db, "SELECT orphaned FROM parity_runs WHERE id = ?", idstr, NULL)));
        json_object_object_add(*report, "finished", json_object_new_boolean(last));
        json_object_object_add(*report, "entries", listed);
    } else {
        json_object_put(listed);
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}

struct json_object *thread_store_parity_last(thread_store *s, const char *owner) {
    struct json_object *o = json_object_new_object();
    pthread_mutex_lock(&s->lock);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, "SELECT id, started_ms, finished_ms, seen, recorded, missing, stale, orphaned, entries FROM parity_runs WHERE owner = ? AND finished_ms IS NOT NULL ORDER BY id DESC LIMIT 1", -1, &stmt, NULL) == SQLITE_OK) {
        thread_db_bind_text(stmt, 1, owner);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            json_object_object_add(o, "run_id", json_object_new_int64(sqlite3_column_int64(stmt, 0)));
            json_object_object_add(o, "started_ms", json_object_new_int64(sqlite3_column_int64(stmt, 1)));
            json_object_object_add(o, "finished_ms", json_object_new_int64(sqlite3_column_int64(stmt, 2)));
            json_object_object_add(o, "seen", json_object_new_int64(sqlite3_column_int64(stmt, 3)));
            json_object_object_add(o, "recorded", json_object_new_int64(sqlite3_column_int64(stmt, 4)));
            json_object_object_add(o, "missing", json_object_new_int64(sqlite3_column_int64(stmt, 5)));
            json_object_object_add(o, "stale", json_object_new_int64(sqlite3_column_int64(stmt, 6)));
            json_object_object_add(o, "orphaned", json_object_new_int64(sqlite3_column_int64(stmt, 7)));
            const char *entries = (const char *)sqlite3_column_text(stmt, 8);
            struct json_object *arr = entries ? mc_json_parse(entries, strlen(entries)) : NULL;
            json_object_object_add(o, "entries", arr ? arr : json_object_new_array());
        }
        sqlite3_finalize(stmt);
    }
    json_object_object_add(o, "files", json_object_new_int64(thread_db_int(s->db, "SELECT COUNT(*) FROM files WHERE owner = ?", owner, NULL)));
    pthread_mutex_unlock(&s->lock);
    return o;
}
