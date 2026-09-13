/* sewnd's side of its socket. A connection carries one operation, named by its
 * first frame: `key.status`, `key.set{key}`, `key.verify`, `turn.start` (sewn/turn.h)
 * and `transcribe.start` (sewn/transcribe.h). Replies are JSON frames:
 * `key.status{present, verified_at, ok?, message?}` or `error{stage, message}`. */
#ifndef MARY_SEWN_SERVER_H
#define MARY_SEWN_SERVER_H

#include <stdbool.h>
#include <stddef.h>

#include "sewn/key.h"
#include "sewn/peer.h"
#include "sewn/transport.h"

/* Checks a key with Mistral: 1 accepted, 0 refused, -errno unreachable. */
typedef int (*sewn_verify_fn)(const char *key, char *message, size_t cap, void *user);

typedef struct sewn_service {
    sewn_key_store keys;
    const char *admin_group;
    sewn_group_fn in_group;
    sewn_verify_fn verify;      /* NULL when built without libcurl */
    void *verify_user;
    sewn_post_stream_fn post_stream;    /* NULL when built without libcurl */
    void *post_stream_user;
    const sewn_ws_ops *ws;              /* NULL when built without libwebsockets */
    void *ws_user;
    int transcribe_wait_ms;             /* how long to wait for Voxtral's final transcript; 0: the default */
} sewn_service;

void sewn_service_init(sewn_service *svc, const char *state_dir);
/* Serves one connection to the end of its operation; the caller closes fd. 0, or
 * -errno when the connection failed before a reply could be sent. */
int sewn_serve_connection(sewn_service *svc, int fd, const sewn_peer *peer);
/* A listening unix socket at `path` with `mode`, replacing a stale socket (never
 * another kind of file). The fd, or -errno. */
int sewn_listen(const char *path, int mode);

#endif
