/* local.sock: the MaryOS-only ops as newline JSON. A request is one line,
 * {"type": <op>, …}; the answer one line, {"type": "<op>.result", …} or
 * {"type": "error", "message": …, "code": <errno name>}. One connection may carry
 * many requests. Fields in [] are optional.
 *
 *   stats                                          → the store's counts (thread_store_stats_json)
 *   schemas                                        → the families with their counts
 *   library {[limit], [after], [document_ids]}     → {groups, has_more}
 *   documents {ids}                                → {documents}
 *   export {[groups], [prefix], [after], [limit]}  → {documents, has_more}
 *   search {query, [entities], [groups], [lanes], [top_k], [request_id], [source]}
 *   deposit {…}                                    → {document_id, partitions, family} (thread_store_deposit_json)
 *   remove {ids | all: true}                       → {removed}
 *   update.group {group, [access], [label], [description], [tags]}
 *   update.document {document_id, [access], [group]}
 *   graph {[entity], [query], [kinds], [hops], [limit], [documents]}
 *   graph.mutate {op, id, [name], [into], [kind]}  → {surviving_id, affected_document_ids}
 *   graph.reextract {document_id}
 *   policy {[set]}                                 → the extraction policy
 *   ledger {[before_id], [kind], [document_id], [request_id], [limit]} → {rows, has_more}
 *   parity                                         → the last finished run
 *   parity.report {[run_id], entries, [last]}      → {run_id, seen, recorded, missing, stale, orphaned, entries}
 *   file.record {path | id}
 *   file.move {from, to}
 *   file.remove {path}                             → {removed}
 *   enrich.drain                                   → {ran}
 *   backup {path}                                  → {path}
 *   checkpoint
 *
 * Every op takes [owner_id], honoured only for the trusted caller (service.h), and
 * [source], the ledger's word for who asked (desktop, threadctl, indexd, maryd, sewnd). */
#ifndef MARY_THREAD_LOCAL_H
#define MARY_THREAD_LOCAL_H

#include "thread/service.h"

#define THREAD_LOCAL_LINE_MAX (16u << 20)

struct json_object;

/* Answers one request. A new reference the caller sends and releases. */
struct json_object *thread_local_handle(thread_caller *caller, struct json_object *request);
/* Serves a connected fd until the peer closes it. 0, or -errno. */
int thread_local_serve(int fd, thread_caller *caller);

#endif
