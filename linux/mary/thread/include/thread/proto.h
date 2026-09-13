/* The store over thread.v1's messages: what threadd's gRPC routes call. Each takes
 * the caller's owner (service.h decides it) and answers a protobuf-c message the
 * caller frees with the matching free. */
#ifndef MARY_THREAD_PROTO_H
#define MARY_THREAD_PROTO_H

#include <stddef.h>

#include "conduit/thread.pb-c.h"
#include "thread/store.h"

/* ThreadQuery.Index: every item with texts becomes a deposit (texts unchunked, tags,
 * entities and relationships as given). 0 with *indexed; the first item's error otherwise
 * (-EINVAL, -EPERM, -errno) with nothing written for it and the rest still tried. */
int thread_store_index(thread_store *store, const char *owner, const Thread__V1__ThreadIndexRequest *request, size_t *indexed);
Thread__V1__ThreadLibraryResponse *thread_store_library(thread_store *store, const char *owner, const Thread__V1__ThreadLibraryRequest *request);
void thread_library_response_free(Thread__V1__ThreadLibraryResponse *response);
Thread__V1__ThreadDocumentsResponse *thread_store_documents(thread_store *store, const char *owner, const Thread__V1__ThreadDocumentsRequest *request);
void thread_documents_response_free(Thread__V1__ThreadDocumentsResponse *response);
Thread__V1__ThreadExportCorpusResponse *thread_store_export(thread_store *store, const char *owner, const Thread__V1__ThreadExportCorpusRequest *request);
void thread_export_response_free(Thread__V1__ThreadExportCorpusResponse *response);
/* ThreadQuery.Search. NULL with *error on a failure (-ENOSYS: no embedder). */
Thread__V1__ThreadSearchResponse *thread_store_search_proto(thread_store *store, const char *owner, const Thread__V1__ThreadSearchRequest *request, int *error);
void thread_search_response_free(Thread__V1__ThreadSearchResponse *response);
Thread__V1__ThreadGraphQueryResponse *thread_store_graph_proto(thread_store *store, const char *owner, const Thread__V1__ThreadGraphQueryRequest *request);
void thread_graph_response_free(Thread__V1__ThreadGraphQueryResponse *response);

#endif
