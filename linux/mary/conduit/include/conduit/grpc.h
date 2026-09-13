/* gRPC's unary calls over HTTP/2 (nghttp2): the bytes grpc-swift puts on the wire,
 * so Conduit's services can be served and called from C, and a Swift Thread node
 * could one day be a peer.
 *
 *   request    HEADERS  :method POST, :scheme http, :path /thread.v1.ThreadQuery/Index,
 *                       content-type application/grpc, te trailers
 *              DATA     0x00 | u32 big-endian length | message
 *   response   HEADERS  :status 200, content-type application/grpc
 *              DATA     0x00 | u32 big-endian length | message
 *              HEADERS  grpc-status, grpc-message                       (trailers)
 *
 * A call that fails before any message is answered with trailers alone. Messages
 * are uncompressed and at most 4 MiB. The HTTP/2 runs in the clear: for now it
 * only crosses unix sockets on one machine; TLS with mutual authentication is the
 * peering milestone (thread/peer.h). Unary only — Conduit's streaming Session and
 * Train are answered UNIMPLEMENTED. Conduit's own session machinery
 * (Sources/Conduit/Client, Server, Session) is not ported. */
#ifndef MARY_CONDUIT_GRPC_H
#define MARY_CONDUIT_GRPC_H

#include <stddef.h>
#include <stdint.h>

#define CONDUIT_MESSAGE_MAX (4u << 20)

enum conduit_status {
    CONDUIT_OK = 0,
    CONDUIT_CANCELLED = 1,
    CONDUIT_UNKNOWN = 2,
    CONDUIT_INVALID_ARGUMENT = 3,
    CONDUIT_DEADLINE_EXCEEDED = 4,
    CONDUIT_NOT_FOUND = 5,
    CONDUIT_PERMISSION_DENIED = 7,
    CONDUIT_RESOURCE_EXHAUSTED = 8,
    CONDUIT_FAILED_PRECONDITION = 9,
    CONDUIT_UNIMPLEMENTED = 12,
    CONDUIT_INTERNAL = 13,
    CONDUIT_UNAVAILABLE = 14,
    CONDUIT_UNAUTHENTICATED = 16,
};

/* What a handler answers. `body` is malloc'd; the server frees it once sent. A
 * non-OK status sends no body. */
typedef struct conduit_reply {
    int status;
    char message[256];
    uint8_t *body;
    size_t body_len;
} conduit_reply;

typedef void (*conduit_handler_fn)(const uint8_t *request, size_t len, conduit_reply *reply, void *user);

typedef struct conduit_route {
    const char *path;           /* "/thread.v1.ThreadQuery/Index" */
    conduit_handler_fn handle;
} conduit_route;

/* Serves gRPC on one connected fd until the peer closes it. A call with no
 * route is answered UNIMPLEMENTED. 0 at a clean end, -EPROTO for a peer that
 * does not speak HTTP/2, or -errno. */
int conduit_serve(int fd, const conduit_route *routes, size_t route_count, void *user);

typedef struct conduit_result {
    int status;
    char message[256];
    uint8_t *body;              /* the response message; the caller frees it */
    size_t body_len;
} conduit_result;

/* One unary call on a connected fd — a connection per call, as MaryThread's
 * ThreadDirectClient opens one. 0 when the call completed (see result->status),
 * -EMSGSIZE for a request over the cap, -ETIMEDOUT, or -errno. */
int conduit_call(int fd, const char *path, const uint8_t *request, size_t len, int timeout_ms, conduit_result *result);

/* A connected unix socket, or -errno. */
int conduit_connect_unix(const char *path);

#endif
