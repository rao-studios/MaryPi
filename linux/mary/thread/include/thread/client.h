/* Mary's client for the local Thread node (MaryThread/ThreadDirectClient.swift), inside the
 * thread package because the Thread is MaryOS's hard drive, not a service Mary is a
 * client of: the calls maryd makes to threadd over Conduit's gRPC, one connection per call as Swift
 * opens one. threadd takes the owner from the socket's peer credentials, so owner_id
 * is sent only to match Swift's wire.
 *
 * The conversation is no longer deposited (the conversation family was retired: Sewn's memory
 * covers it); what remains is the turn's request id, minted here as the Mac mints it, and the
 * library and documents reads.
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
#define THREAD_CLIENT_SCOPE "personal"
#define THREAD_CLIENT_TIMEOUT_MS 5000

/* $THREAD_SOCKET, or THREAD_CLIENT_SOCKET_PATH. */
const char *thread_client_default_socket(void);

/* "mary-turn-<started_ms>-<4 hex>": a turn's request id (the document id its conversation record once had). */
void thread_turn_document_id(int64_t started_ms, char *out, size_t cap);

/* ThreadLibrary.Library: the caller's groups, `limit` at most (0: all) after `after_id`
 * (NULL: from the start). *out is freed with thread__v1__thread_library_response__free_unpacked. */
int thread_client_library(const char *socket_path, int limit, const char *after_id, int timeout_ms,
               Thread__V1__ThreadLibraryResponse **out, int *status);

/* ThreadLibrary.Documents for `ids`. *out is freed with
 * thread__v1__thread_documents_response__free_unpacked. */
int thread_client_documents(const char *socket_path, const char *const *ids, size_t count, int timeout_ms,
                 Thread__V1__ThreadDocumentsResponse **out, int *status);

#endif
