#include "thread/db.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "common/log.h"

static const char SCHEMA[] =
    "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS groups(id TEXT PRIMARY KEY, owner TEXT NOT NULL, label TEXT NOT NULL DEFAULT '',"
    "  access TEXT NOT NULL DEFAULT 'restricted', description TEXT NOT NULL DEFAULT '', tags TEXT NOT NULL DEFAULT '[]',"
    "  created_at INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS groups_owner ON groups(owner, id);"
    "CREATE TABLE IF NOT EXISTS documents(id TEXT PRIMARY KEY, owner TEXT NOT NULL,"
    "  group_id TEXT REFERENCES groups(id) ON DELETE SET NULL, family TEXT NOT NULL DEFAULT 'unknown',"
    "  name TEXT NOT NULL DEFAULT '', media_type TEXT NOT NULL DEFAULT 'text', access TEXT NOT NULL DEFAULT 'restricted',"
    "  metadata BLOB, content_hash TEXT NOT NULL DEFAULT '', tags TEXT NOT NULL DEFAULT '[]', revision INTEGER NOT NULL DEFAULT 1,"
    "  enrich_state TEXT NOT NULL DEFAULT 'pending', created_at INTEGER NOT NULL, updated_ms INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS documents_owner ON documents(owner, id);"
    "CREATE INDEX IF NOT EXISTS documents_group ON documents(group_id);"
    "CREATE INDEX IF NOT EXISTS documents_family ON documents(family, updated_ms);"
    "CREATE TABLE IF NOT EXISTS partitions(rowid INTEGER PRIMARY KEY,"
    "  document_id TEXT NOT NULL REFERENCES documents(id) ON DELETE CASCADE ON UPDATE CASCADE,"
    "  seq INTEGER NOT NULL, id TEXT, text TEXT NOT NULL, media_type TEXT NOT NULL DEFAULT 'text',"
    "  embedding BLOB, dim INTEGER NOT NULL DEFAULT 0, UNIQUE(document_id, seq));"
    "CREATE INDEX IF NOT EXISTS partitions_id ON partitions(id);"
    "CREATE TABLE IF NOT EXISTS entities(id TEXT PRIMARY KEY, name TEXT NOT NULL, name_norm TEXT NOT NULL, kind TEXT NOT NULL,"
    "  mention_count INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS entities_norm ON entities(name_norm);"
    "CREATE INDEX IF NOT EXISTS entities_kind ON entities(kind, mention_count);"
    "CREATE TABLE IF NOT EXISTS entity_documents(entity_id TEXT NOT NULL REFERENCES entities(id) ON DELETE CASCADE,"
    "  document_id TEXT NOT NULL REFERENCES documents(id) ON DELETE CASCADE ON UPDATE CASCADE,"
    "  position INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(entity_id, document_id));"
    "CREATE INDEX IF NOT EXISTS entity_documents_doc ON entity_documents(document_id, position);"
    "CREATE TABLE IF NOT EXISTS entity_tokens(token TEXT NOT NULL, entity_id TEXT NOT NULL REFERENCES entities(id) ON DELETE CASCADE,"
    "  PRIMARY KEY(token, entity_id));"
    "CREATE TABLE IF NOT EXISTS predicates(id TEXT PRIMARY KEY, name TEXT NOT NULL, embedding BLOB,"
    "  relationship_count INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS relationships(id TEXT PRIMARY KEY,"
    "  subject_id TEXT NOT NULL REFERENCES entities(id) ON DELETE CASCADE, predicate TEXT NOT NULL,"
    "  predicate_id TEXT NOT NULL, object_id TEXT NOT NULL REFERENCES entities(id) ON DELETE CASCADE,"
    "  weight INTEGER NOT NULL DEFAULT 0, embedding BLOB);"
    "CREATE INDEX IF NOT EXISTS relationships_subject ON relationships(subject_id);"
    "CREATE INDEX IF NOT EXISTS relationships_object ON relationships(object_id);"
    "CREATE INDEX IF NOT EXISTS relationships_predicate ON relationships(predicate_id);"
    "CREATE TABLE IF NOT EXISTS relationship_documents(relationship_id TEXT NOT NULL REFERENCES relationships(id) ON DELETE CASCADE,"
    "  document_id TEXT NOT NULL REFERENCES documents(id) ON DELETE CASCADE ON UPDATE CASCADE,"
    "  PRIMARY KEY(relationship_id, document_id));"
    "CREATE INDEX IF NOT EXISTS relationship_documents_doc ON relationship_documents(document_id);"
    "CREATE TABLE IF NOT EXISTS document_stats(document_id TEXT PRIMARY KEY REFERENCES documents(id) ON DELETE CASCADE ON UPDATE CASCADE,"
    "  retrieval_count INTEGER NOT NULL DEFAULT 0, last_retrieved_ms INTEGER);"
    "CREATE TABLE IF NOT EXISTS files(id TEXT PRIMARY KEY REFERENCES documents(id) ON DELETE CASCADE ON UPDATE CASCADE,"
    "  owner TEXT NOT NULL, path TEXT NOT NULL, kind TEXT NOT NULL DEFAULT 'document', size INTEGER NOT NULL DEFAULT 0,"
    "  mtime_ms INTEGER NOT NULL DEFAULT 0, content_hash TEXT NOT NULL DEFAULT '', text_indexed INTEGER NOT NULL DEFAULT 0,"
    "  seen_ms INTEGER NOT NULL DEFAULT 0, UNIQUE(owner, path));"
    "CREATE TABLE IF NOT EXISTS parity_runs(id INTEGER PRIMARY KEY, owner TEXT NOT NULL, started_ms INTEGER NOT NULL,"
    "  finished_ms INTEGER, seen INTEGER NOT NULL DEFAULT 0, recorded INTEGER NOT NULL DEFAULT 0, missing INTEGER NOT NULL DEFAULT 0,"
    "  stale INTEGER NOT NULL DEFAULT 0, orphaned INTEGER NOT NULL DEFAULT 0, entries TEXT NOT NULL DEFAULT '[]');"
    "CREATE TABLE IF NOT EXISTS jobs(id INTEGER PRIMARY KEY,"
    "  document_id TEXT NOT NULL UNIQUE REFERENCES documents(id) ON DELETE CASCADE ON UPDATE CASCADE,"
    "  revision INTEGER NOT NULL, step TEXT NOT NULL, payload TEXT NOT NULL DEFAULT '{}', needs_extraction INTEGER NOT NULL DEFAULT 0,"
    "  attempts INTEGER NOT NULL DEFAULT 0, next_ms INTEGER NOT NULL DEFAULT 0, last_error TEXT, created_ms INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS jobs_next ON jobs(next_ms);"
    "CREATE TABLE IF NOT EXISTS ledger(id INTEGER PRIMARY KEY, at_ms INTEGER NOT NULL, kind TEXT NOT NULL, source TEXT NOT NULL,"
    "  document_id TEXT, group_id TEXT, request_id TEXT, count INTEGER NOT NULL DEFAULT 0, ms INTEGER NOT NULL DEFAULT 0, detail TEXT);"
    "CREATE INDEX IF NOT EXISTS ledger_at ON ledger(at_ms);"
    "CREATE INDEX IF NOT EXISTS ledger_doc ON ledger(document_id);"
    "CREATE INDEX IF NOT EXISTS ledger_kind ON ledger(kind, id);"
    "CREATE INDEX IF NOT EXISTS ledger_request ON ledger(request_id);"
    "CREATE TABLE IF NOT EXISTS ledger_documents(ledger_id INTEGER NOT NULL REFERENCES ledger(id) ON DELETE CASCADE,"
    "  rank INTEGER NOT NULL, document_id TEXT NOT NULL, partition_id TEXT, score REAL, PRIMARY KEY(ledger_id, rank));";

static int fail(sqlite3 *db, const char *what) {
    mc_log(MC_LOG_ERROR, "thread.db: %s: %s", what, db ? sqlite3_errmsg(db) : "no database");
    return -EIO;
}

int thread_db_exec(sqlite3 *db, const char *sql) {
    char *message = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &message) != SQLITE_OK) {
        mc_log(MC_LOG_ERROR, "thread.db: %s", message ? message : sqlite3_errmsg(db));
        sqlite3_free(message);
        return -EIO;
    }
    return 0;
}

int thread_db_begin(sqlite3 *db) { return thread_db_exec(db, "BEGIN IMMEDIATE"); }
int thread_db_commit(sqlite3 *db) { return thread_db_exec(db, "COMMIT"); }
int thread_db_rollback(sqlite3 *db) { return thread_db_exec(db, "ROLLBACK"); }
int thread_db_checkpoint(sqlite3 *db) { return thread_db_exec(db, "PRAGMA wal_checkpoint(TRUNCATE)"); }

int thread_db_backup(sqlite3 *db, const char *path) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "VACUUM INTO ?", -1, &stmt, NULL) != SQLITE_OK) return fail(db, "backup");
    thread_db_bind_text(stmt, 1, path);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : fail(db, "backup");
    sqlite3_finalize(stmt);
    return rc;
}

int thread_db_open(const char *path, sqlite3 **out) {
    *out = NULL;
    sqlite3 *db = NULL;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path, &db, flags, NULL) != SQLITE_OK) {
        int rc = fail(db, "open");
        sqlite3_close(db);
        return rc;
    }
    /* private before the WAL and shm files inherit the mode (a memory database has no path) */
    if (strcmp(path, ":memory:") != 0) chmod(path, 0600);
    sqlite3_busy_timeout(db, 5000);
    int rc = thread_db_exec(db, "PRAGMA page_size=8192; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; "
                                "PRAGMA foreign_keys=ON; PRAGMA temp_store=MEMORY;");
    if (rc == 0) rc = thread_db_exec(db, SCHEMA);
    if (rc == 0) {
        int64_t version = thread_db_int(db, "PRAGMA user_version", NULL, NULL);
        if (version < THREAD_DB_USER_VERSION) {
            char sql[64];
            snprintf(sql, sizeof sql, "PRAGMA user_version=%d", THREAD_DB_USER_VERSION);
            rc = thread_db_exec(db, sql);
        }
    }
    if (rc) {
        sqlite3_close(db);
        return rc;
    }
    *out = db;
    return 0;
}

void thread_db_close(sqlite3 *db) {
    if (db) sqlite3_close(db);
}

void thread_db_bind_text(sqlite3_stmt *stmt, int index, const char *text) {
    if (text) sqlite3_bind_text(stmt, index, text, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, index);
}

void thread_db_bind_floats(sqlite3_stmt *stmt, int index, const float *v, size_t dim) {
    if (v && dim) sqlite3_bind_blob(stmt, index, v, (int)(dim * sizeof *v), SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, index);
}

float *thread_db_column_floats(sqlite3_stmt *stmt, int column, size_t dim) {
    if (sqlite3_column_type(stmt, column) != SQLITE_BLOB) return NULL;
    size_t bytes = (size_t)sqlite3_column_bytes(stmt, column);
    if (bytes != dim * sizeof(float)) return NULL;
    float *v = malloc(bytes);
    if (v) memcpy(v, sqlite3_column_blob(stmt, column), bytes);
    return v;
}

int64_t thread_db_int(sqlite3 *db, const char *sql, const char *arg1, const char *arg2) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fail(db, sql);
        return -1;
    }
    if (arg1) thread_db_bind_text(stmt, 1, arg1);
    if (arg2) thread_db_bind_text(stmt, 2, arg2);
    int64_t value = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

char *thread_db_text(sqlite3 *db, const char *sql, const char *arg1, const char *arg2) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fail(db, sql);
        return NULL;
    }
    if (arg1) thread_db_bind_text(stmt, 1, arg1);
    if (arg2) thread_db_bind_text(stmt, 2, arg2);
    char *value = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        const unsigned char *t = sqlite3_column_text(stmt, 0);
        value = strdup(t ? (const char *)t : "");
    }
    sqlite3_finalize(stmt);
    return value;
}

char *thread_db_meta_get(sqlite3 *db, const char *key) {
    return thread_db_text(db, "SELECT value FROM meta WHERE key = ?", key, NULL);
}

int thread_db_meta_set(sqlite3 *db, const char *key, const char *value) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)", -1, &stmt, NULL) != SQLITE_OK) return fail(db, "meta");
    thread_db_bind_text(stmt, 1, key);
    thread_db_bind_text(stmt, 2, value);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : fail(db, "meta");
    sqlite3_finalize(stmt);
    return rc;
}
