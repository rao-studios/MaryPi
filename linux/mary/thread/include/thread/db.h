/* thread.db: the one file that is the hard drive's memory. SQLite in WAL mode with
 * 8 KiB pages (an embedding and its row fit one page), foreign keys on, a 5 s busy
 * timeout, opened with the full mutex behind the store's own lock. Every table the
 * Swift node keeps as plist snapshots (Sources/Database) is one here, plus what
 * MaryOS adds: the ledger, the file parity rows, the enrichment jobs. */
#ifndef MARY_THREAD_DB_H
#define MARY_THREAD_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sqlite3.h>

#define THREAD_DB_FILE "thread.db"
#define THREAD_DB_USER_VERSION 1
#define THREAD_EMBEDDING_DIM 1024
#define THREAD_EMBEDDING_MODEL "mistral-embed"

/* Opens (creating and migrating) the database at `path`. 0, or -errno / -EIO with
 * sqlite's message logged. */
int thread_db_open(const char *path, sqlite3 **out);
void thread_db_close(sqlite3 *db);

/* Runs one or more statements. 0, or -EIO (logged). */
int thread_db_exec(sqlite3 *db, const char *sql);
int thread_db_begin(sqlite3 *db);
int thread_db_commit(sqlite3 *db);
int thread_db_rollback(sqlite3 *db);
/* Truncates the WAL back into the file, so thread.db alone is the backup. */
int thread_db_checkpoint(sqlite3 *db);
/* VACUUM INTO `path`. */
int thread_db_backup(sqlite3 *db, const char *path);

/* meta: small named values. Heap result (the caller frees), NULL when absent. */
char *thread_db_meta_get(sqlite3 *db, const char *key);
int thread_db_meta_set(sqlite3 *db, const char *key, const char *value);

/* One-row helpers. -1 / NULL on error or no row. Text results are heap. */
int64_t thread_db_int(sqlite3 *db, const char *sql, const char *arg1, const char *arg2);
char *thread_db_text(sqlite3 *db, const char *sql, const char *arg1, const char *arg2);
/* Binds text or NULL (SQLITE_STATIC copies are not needed: bound transient). */
void thread_db_bind_text(sqlite3_stmt *stmt, int index, const char *text);
/* A float32 LE blob of `dim` floats, or NULL. */
void thread_db_bind_floats(sqlite3_stmt *stmt, int index, const float *v, size_t dim);
/* Copies a float blob column into a new array (the caller frees); NULL when the column is NULL or its size is not dim × 4. */
float *thread_db_column_floats(sqlite3_stmt *stmt, int column, size_t dim);

#endif
