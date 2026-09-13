/* Parity between the home and the Thread: walk every file, report each page of
 * {path, size, mtime_ms, hash} to threadd, deposit the ones it calls missing or
 * stale, and let the last page remove the records whose files are gone. Runs when
 * indexd starts (files changed while it was down), every few hours, and on
 * `indexd --once`. */
#ifndef MARY_INDEX_RECONCILE_H
#define MARY_INDEX_RECONCILE_H

#include <stddef.h>
#include <stdint.h>

#include "index/client.h"

#define IX_PARITY_PAGE 200

typedef struct ix_reconcile_stats {
    int64_t run_id;
    long seen;          /* files walked */
    long recorded;      /* already right */
    long missing;
    long stale;
    long orphaned;      /* records removed */
    long deposited;     /* missing + stale that were written */
    long failed;        /* deposits threadd refused or files that vanished meanwhile */
    int64_t ms;
} ix_reconcile_stats;

/* 0, or -errno when threadd could not be reached (nothing partial is left behind:
 * an unfinished run is dropped by the next one). */
int ix_reconcile(const ix_client *client, const char *root, const char *owner, ix_reconcile_stats *stats, char *message, size_t cap);

#endif
