/* threadd's gRPC methods: ThreadQuery.Index, ThreadLibrary.Library and
 * ThreadLibrary.Documents, served by conduit over one connection. Every other
 * thread.v1 method is answered UNIMPLEMENTED. */
#ifndef MARY_THREAD_SERVICE_H
#define MARY_THREAD_SERVICE_H

#include <stddef.h>

#include "conduit/grpc.h"
#include "thread/store.h"

#define THREAD_SOCKET_PATH "/run/thread/thread.sock"
#define THREAD_PATH_INDEX "/thread.v1.ThreadQuery/Index"
#define THREAD_PATH_LIBRARY "/thread.v1.ThreadLibrary/Library"
#define THREAD_PATH_DOCUMENTS "/thread.v1.ThreadLibrary/Documents"

/* One connection: the store, and who is calling (a login name from peer credentials). */
typedef struct thread_caller {
    thread_store *store;
    char owner[64];
} thread_caller;

extern const conduit_route thread_routes[];
extern const size_t thread_route_count;

#endif
