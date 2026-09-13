/* indexd's calls into threadd, over /run/thread/local.sock (thread/local.h): the
 * deposit of a record, a move, a removal, and the pages of a parity report. threadd
 * takes the owner from the connection, so indexd names none. */
#ifndef MARY_INDEX_CLIENT_H
#define MARY_INDEX_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IX_THREAD_SOCKET "/run/thread/local.sock"
#define IX_CALL_TIMEOUT_MS 30000

struct json_object;

typedef struct ix_client {
    const char *socket_path;    /* NULL: $THREAD_LOCAL_SOCKET, else IX_THREAD_SOCKET */
} ix_client;

const char *ix_client_socket(const ix_client *c);
/* 0, or -errno / -EIO with `message` (threadd's own words). */
int ix_call(const ix_client *c, struct json_object *request, struct json_object **reply, char *message, size_t cap);
/* deposit: the record (ix_record); the document id is copied out when given. */
int ix_deposit(const ix_client *c, struct json_object *record, char *document_id, size_t cap, char *message, size_t msg_cap);
int ix_file_move(const ix_client *c, const char *from, const char *to, char *message, size_t cap);
/* *removed says whether a record existed. */
int ix_file_remove(const ix_client *c, const char *path, bool *removed, char *message, size_t cap);
/* One page of parity: *run_id 0 opens a run; `last` closes it. *report is the reply
 * (run_id, seen, recorded, missing, stale, orphaned, entries[{path, status}]). */
int ix_parity_report(const ix_client *c, int64_t *run_id, struct json_object *entries, bool last, struct json_object **report, char *message, size_t cap);

#endif
