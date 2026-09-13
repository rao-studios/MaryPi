/* Mary's client for the local Thread node (MaryThread/ThreadDirectClient.swift), inside the
 * thread package because the Thread is MaryOS's hard drive, not a service Mary is a
 * client of: the calls maryd makes to threadd over Conduit's gRPC, one connection per call as Swift
 * opens one. threadd takes the owner from the socket's peer credentials, so owner_id
 * is sent only to match Swift's wire.
 *
 * Every spoken or typed turn is deposited as one document in Mary's conversations group:
 *
 *   group     conversation-<owner> ("Conversation"; the conversation lane), scope "personal"
 *   document  mary-turn-<started ms>-<4 hex>
 *   texts     [what the user said, what Mary answered]
 *   metadata  {family: "conversation", source, model, started_ms, ended_ms, cancelled}  (JSON bytes)
 *
 * Results: 0; -errno when threadd cannot be reached or the call fails on the way
 * (-ETIMEDOUT, -EMSGSIZE); -EPROTO when threadd answered with a gRPC status other
 * than OK (*status, when given, says which) or with bytes that do not unpack. */
#ifndef MARY_THREAD_CLIENT_H
#define MARY_THREAD_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "conduit/thread.pb-c.h"

/* The same socket service.h serves; spelled here so the client needs no server header. */
#define THREAD_CLIENT_SOCKET_PATH "/run/thread/thread.sock"
/* The conversation lane's group: conversation-<owner> (thread/families.h). */
#define THREAD_CLIENT_GROUP_PREFIX "conversation-"
#define THREAD_CLIENT_GROUP_LABEL "Conversation"
#define THREAD_CLIENT_SCOPE "personal"
#define THREAD_CLIENT_TIMEOUT_MS 5000

/* $THREAD_SOCKET, or THREAD_CLIENT_SOCKET_PATH. */
const char *thread_client_default_socket(void);

typedef struct thread_turn {
    const char *owner_id;       /* the login name; threadd uses its own reading of it */
    const char *user_text;
    const char *reply;          /* what was shown and spoken; may be partial when cancelled */
    const char *source;         /* "voice" or "typed" */
    const char *model;          /* the chat model, or NULL */
    int64_t started_ms;         /* wall clock */
    int64_t ended_ms;
    bool cancelled;
} thread_turn;

/* "mary-turn-<started_ms>-<4 hex>", the id a turn's document gets. */
void thread_turn_document_id(int64_t started_ms, char *out, size_t cap);

/* The ThreadIndexRequest for one turn, packed. Heap; the caller frees. NULL when the
 * turn has no user text or out of memory. document_id is copied out when given. */
uint8_t *thread_turn_index_request(const thread_turn *turn, size_t *len, char *document_id, size_t cap);

/* ThreadQuery.Index with that request. The document id is copied out when given. */
int thread_client_deposit_turn(const char *socket_path, const thread_turn *turn, int timeout_ms, char *document_id, size_t cap, int *status);

/* ThreadLibrary.Library: the caller's groups, `limit` at most (0: all) after `after_id`
 * (NULL: from the start). *out is freed with thread__v1__thread_library_response__free_unpacked. */
int thread_client_library(const char *socket_path, int limit, const char *after_id, int timeout_ms,
               Thread__V1__ThreadLibraryResponse **out, int *status);

/* ThreadLibrary.Documents for `ids`. *out is freed with
 * thread__v1__thread_documents_response__free_unpacked. */
int thread_client_documents(const char *socket_path, const char *const *ids, size_t count, int timeout_ms,
                 Thread__V1__ThreadDocumentsResponse **out, int *status);

#endif
