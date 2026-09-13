#include "thread/ledger.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "common/io.h"
#include "common/json.h"
#include "common/log.h"
#include "thread/db.h"

static int inserts_since_roll;

int64_t thread_ledger_record(sqlite3 *db, const thread_ledger_row *row) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "INSERT INTO ledger(at_ms, kind, source, document_id, group_id, request_id, count, ms, detail) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)",
                           -1, &stmt, NULL) != SQLITE_OK) {
        mc_log(MC_LOG_ERROR, "ledger: %s", sqlite3_errmsg(db));
        return -EIO;
    }
    sqlite3_bind_int64(stmt, 1, mc_wall_ms());
    thread_db_bind_text(stmt, 2, row->kind ? row->kind : "index");
    thread_db_bind_text(stmt, 3, row->source ? row->source : "threadd");
    thread_db_bind_text(stmt, 4, row->document_id);
    thread_db_bind_text(stmt, 5, row->group_id);
    thread_db_bind_text(stmt, 6, row->request_id);
    sqlite3_bind_int64(stmt, 7, row->count);
    sqlite3_bind_int64(stmt, 8, row->ms);
    size_t len = 0;
    const char *detail = row->detail ? mc_json_compact(row->detail, &len) : NULL;
    thread_db_bind_text(stmt, 9, detail);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        mc_log(MC_LOG_ERROR, "ledger: %s", sqlite3_errmsg(db));
        return -EIO;
    }
    int64_t id = sqlite3_last_insert_rowid(db);
    if (++inserts_since_roll >= 256) {
        inserts_since_roll = 0;
        thread_ledger_roll(db);
    }
    return id;
}

int thread_ledger_record_hit(sqlite3 *db, int64_t ledger_id, int rank, const char *document_id, const char *partition_id, double score) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO ledger_documents(ledger_id, rank, document_id, partition_id, score) VALUES(?, ?, ?, ?, ?)",
                           -1, &stmt, NULL) != SQLITE_OK) return -EIO;
    sqlite3_bind_int64(stmt, 1, ledger_id);
    sqlite3_bind_int(stmt, 2, rank);
    thread_db_bind_text(stmt, 3, document_id);
    thread_db_bind_text(stmt, 4, partition_id);
    sqlite3_bind_double(stmt, 5, score);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -EIO;
    sqlite3_finalize(stmt);
    return rc;
}

int thread_ledger_roll(sqlite3 *db) {
    char sql[160];
    snprintf(sql, sizeof sql, "DELETE FROM ledger WHERE id <= (SELECT id FROM ledger ORDER BY id DESC LIMIT 1 OFFSET %d)", THREAD_LEDGER_KEEP);
    return thread_db_exec(db, sql);
}

int64_t thread_ledger_count(sqlite3 *db) { return thread_db_int(db, "SELECT COUNT(*) FROM ledger", NULL, NULL); }

static void add_text(struct json_object *o, const char *key, sqlite3_stmt *stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) json_object_object_add(o, key, NULL);
    else json_object_object_add(o, key, json_object_new_string((const char *)sqlite3_column_text(stmt, col)));
}

int thread_ledger_page(sqlite3 *db, const thread_ledger_filter *filter, struct json_object **rows, int *has_more) {
    *rows = json_object_new_array();
    *has_more = 0;
    int limit = filter->limit > 0 ? filter->limit : 50;
    if (limit > THREAD_LEDGER_PAGE_MAX) limit = THREAD_LEDGER_PAGE_MAX;
    char sql[512];
    snprintf(sql, sizeof sql,
             "SELECT id, at_ms, kind, source, document_id, group_id, request_id, count, ms, detail FROM ledger "
             "WHERE (?1 = 0 OR id < ?1) AND (?2 IS NULL OR kind = ?2) AND (?3 IS NULL OR document_id = ?3) AND (?4 IS NULL OR request_id = ?4) "
             "ORDER BY id DESC LIMIT %d", limit + 1);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        mc_log(MC_LOG_ERROR, "ledger: %s", sqlite3_errmsg(db));
        return -EIO;
    }
    sqlite3_bind_int64(stmt, 1, filter->before_id);
    thread_db_bind_text(stmt, 2, filter->kind);
    thread_db_bind_text(stmt, 3, filter->document_id);
    thread_db_bind_text(stmt, 4, filter->request_id);
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (n == limit) {
            *has_more = 1;
            break;
        }
        struct json_object *o = json_object_new_object();
        int64_t id = sqlite3_column_int64(stmt, 0);
        json_object_object_add(o, "id", json_object_new_int64(id));
        json_object_object_add(o, "at_ms", json_object_new_int64(sqlite3_column_int64(stmt, 1)));
        add_text(o, "kind", stmt, 2);
        add_text(o, "source", stmt, 3);
        add_text(o, "document_id", stmt, 4);
        add_text(o, "group_id", stmt, 5);
        add_text(o, "request_id", stmt, 6);
        json_object_object_add(o, "count", json_object_new_int64(sqlite3_column_int64(stmt, 7)));
        json_object_object_add(o, "ms", json_object_new_int64(sqlite3_column_int64(stmt, 8)));
        const char *detail = (const char *)sqlite3_column_text(stmt, 9);
        struct json_object *d = detail ? mc_json_parse(detail, strlen(detail)) : NULL;
        json_object_object_add(o, "detail", d);
        struct json_object *docs = json_object_new_array();
        sqlite3_stmt *ds = NULL;
        if (sqlite3_prepare_v2(db, "SELECT rank, document_id, partition_id, score FROM ledger_documents WHERE ledger_id = ? ORDER BY rank", -1, &ds, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(ds, 1, id);
            while (sqlite3_step(ds) == SQLITE_ROW) {
                struct json_object *h = json_object_new_object();
                json_object_object_add(h, "rank", json_object_new_int(sqlite3_column_int(ds, 0)));
                add_text(h, "document_id", ds, 1);
                add_text(h, "partition_id", ds, 2);
                json_object_object_add(h, "score", json_object_new_double(sqlite3_column_double(ds, 3)));
                json_object_array_add(docs, h);
            }
            sqlite3_finalize(ds);
        }
        json_object_object_add(o, "documents", docs);
        json_object_array_add(*rows, o);
        n++;
    }
    sqlite3_finalize(stmt);
    return 0;
}
