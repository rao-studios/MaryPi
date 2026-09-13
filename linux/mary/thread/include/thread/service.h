/* threadd's two sockets.
 *
 *   thread.sock   Conduit's thread.v1 over gRPC, byte for byte the Mac's wire — served
 *                 in full: ThreadQuery (Index, Search, Remove), ThreadLibrary (Library,
 *                 Documents, ExportCorpus), ThreadGraph (Query), ThreadUpdate (UpdateGroup,
 *                 UpdateDocument, Stats). maryd, sewnd and, one day, peers.
 *   local.sock    newline JSON — {"type": …} in, one {"type": "<op>.result", …} or
 *                 {"type": "error", …} line out — the MaryOS-only ops: the desktop's Thread
 *                 app, threadctl and indexd (thread/local.h lists them).
 *
 * Both take the owner from the socket's peer credentials. One caller is trusted the
 * way Sewn is the Mac's mothership: the `sewn` user may name the owner it writes for,
 * so auto-memory lands in the user's own groups. */
#ifndef MARY_THREAD_SERVICE_H
#define MARY_THREAD_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include "conduit/grpc.h"
#include "thread/store.h"

#define THREAD_SOCKET_PATH "/run/thread/thread.sock"
#define THREAD_LOCAL_SOCKET_PATH "/run/thread/local.sock"
#define THREAD_PATH_INDEX "/thread.v1.ThreadQuery/Index"
#define THREAD_PATH_SEARCH "/thread.v1.ThreadQuery/Search"
#define THREAD_PATH_REMOVE "/thread.v1.ThreadQuery/Remove"
#define THREAD_PATH_LIBRARY "/thread.v1.ThreadLibrary/Library"
#define THREAD_PATH_DOCUMENTS "/thread.v1.ThreadLibrary/Documents"
#define THREAD_PATH_EXPORT "/thread.v1.ThreadLibrary/ExportCorpus"
#define THREAD_PATH_GRAPH "/thread.v1.ThreadGraph/Query"
#define THREAD_PATH_UPDATE_GROUP "/thread.v1.ThreadUpdate/UpdateGroup"
#define THREAD_PATH_UPDATE_DOCUMENT "/thread.v1.ThreadUpdate/UpdateDocument"
#define THREAD_PATH_STATS "/thread.v1.ThreadUpdate/Stats"

/* One connection: the store, who is calling (a login name from peer credentials), and
 * whether the caller may name another owner. */
typedef struct thread_caller {
    thread_store *store;
    char owner[64];
    bool trusted;
} thread_caller;

/* The owner a request is served for: the request's owner_id when the caller is trusted
 * and named one, else the caller. (local.c, so the JSON side links without protobuf.) */
const char *thread_caller_owner(const thread_caller *caller, const char *requested);

extern const conduit_route thread_routes[];
extern const size_t thread_route_count;

#endif
