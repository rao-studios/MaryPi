/* Thread's memory on MaryOS: documents grouped per owner, kept as private JSON
 * files under the state directory —
 *
 *   node-id                      this node's UUID, made on first open
 *   groups/<group-id>.json       {id, label, owner_id, created_at, documents: [ids]}
 *   documents/<document-id>.json {id, owner_id, group_id, name, media_type, created_at, metadata, texts}
 *
 * Swift Thread keeps property lists, a partition table of embeddings and a
 * knowledge graph (Sources/Database); none of that is ported yet (PORTING.md
 * deviation 7). `owner` is always the caller as the kernel reports it, never the
 * request's owner_id, and nobody may replace or read another owner's documents.
 * Calls are serialized. */
#ifndef MARY_THREAD_STORE_H
#define MARY_THREAD_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "conduit/thread.pb-c.h"

#define THREAD_STATE_DIR "/var/lib/thread"
#define THREAD_ID_MAX 200

typedef struct thread_store thread_store;

/* Creates the directories, and node-id on first open. NULL with *error set. */
thread_store *thread_store_open(const char *dir, int *error);
void thread_store_close(thread_store *store);
const char *thread_store_node_id(const thread_store *store);

/* An id fit to be a file name: 1…200 bytes of [A-Za-z0-9._:-], not "." or "..". */
bool thread_id_valid(const char *id);

/* ThreadQuery.Index (Conduit/ThreadQueryServiceImpl.swift): every item with texts
 * is written, replacing an earlier document of this owner with the same id and
 * keeping its created_at, and joins the request's group (created on first use,
 * relabelled when a label is given; a document moves out of its old group).
 * Items without texts are skipped. 0 with *indexed; -EINVAL for an id that is not
 * valid; -EPERM when an id or the group belongs to another owner; -errno. Nothing
 * is written unless every item is acceptable. */
int thread_store_index(thread_store *store, const char *owner, const Thread__V1__ThreadIndexRequest *request, size_t *indexed);

/* ThreadLibrary.Library (Conduit/ThreadLibraryServiceImpl.swift): the owner's groups
 * sorted by id, after `after_id`, at most `limit` (0: all) with has_more — or, when
 * document_ids is not empty, the owner's groups holding any of them. MaryOS has no
 * publicly available groups, so include_available changes nothing. */
Thread__V1__ThreadLibraryResponse *thread_store_library(thread_store *store, const char *owner, const Thread__V1__ThreadLibraryRequest *request);
void thread_library_response_free(Thread__V1__ThreadLibraryResponse *response);

/* ThreadLibrary.Documents: the owner's documents among document_ids, in request
 * order; unknown, invalid or other owners' ids are omitted. */
Thread__V1__ThreadDocumentsResponse *thread_store_documents(thread_store *store, const char *owner, const Thread__V1__ThreadDocumentsRequest *request);
void thread_documents_response_free(Thread__V1__ThreadDocumentsResponse *response);

#endif
