/* The ledger: what was deposited, retrieved, removed, embedded, extracted,
 * reconciled or repaired, and by whom — the Thread app's Ledger tab, and what
 * the Retrieval tab joins a turn's request id to. The Swift node keeps no such
 * log (its "ledger" is Gita's token ledger); Mary's RetrievalTraceLedger lives in
 * the app. Here it is a table, rolled to the newest 10,000 rows. */
#ifndef MARY_THREAD_LEDGER_H
#define MARY_THREAD_LEDGER_H

#include <stddef.h>
#include <stdint.h>

#include <sqlite3.h>

#define THREAD_LEDGER_KEEP 10000
#define THREAD_LEDGER_PAGE_MAX 500

struct json_object;

typedef struct thread_ledger_row {
    const char *kind;           /* index | search | remove | deposit | extract | embed | reconcile | mutation */
    const char *source;         /* maryd | indexd | sewnd | threadctl | threadd | grpc | desktop */
    const char *document_id;    /* or NULL */
    const char *group_id;
    const char *request_id;
    int64_t count;
    int64_t ms;
    struct json_object *detail; /* borrowed; or NULL */
} thread_ledger_row;

/* The new row's id, or a negative errno. */
int64_t thread_ledger_record(sqlite3 *db, const thread_ledger_row *row);
/* What a search returned, in rank order. */
int thread_ledger_record_hit(sqlite3 *db, int64_t ledger_id, int rank, const char *document_id, const char *partition_id, double score);

typedef struct thread_ledger_filter {
    int64_t before_id;          /* 0: from the newest */
    const char *kind;           /* or NULL */
    const char *document_id;
    const char *request_id;
    int limit;                  /* ≤ 500; 0 = 50 */
} thread_ledger_filter;

/* Rows newest first, each {id, at_ms, kind, source, document_id, group_id, request_id,
 * count, ms, detail, documents:[{rank, document_id, partition_id, score}]}. */
int thread_ledger_page(sqlite3 *db, const thread_ledger_filter *filter, struct json_object **rows, int *has_more);
/* Drops everything but the newest THREAD_LEDGER_KEEP rows. */
int thread_ledger_roll(sqlite3 *db);
int64_t thread_ledger_count(sqlite3 *db);

#endif
