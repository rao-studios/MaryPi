/* sewnd's side of its socket. A connection carries one operation, named by its
 * first frame:
 *
 *   key.status | key.set{key} | key.verify                       the key (sewn/key.h)
 *   turn.start{request, tts}                                     the spoken turn (sewn/turn.h)
 *   complete{messages, tools?, instructions?, provider?, scope?} the skills lane (sewn/complete.h)
 *   embed{texts[], purpose?}                                     for threadd (sewn/embed.h)
 *   graph.extract{texts[], prompt}                               for threadd (sewn/complete.h)
 *   summarize{messages[], recent?}                               the auto-memory note (sewn/memory.h)
 *   transcribe.start                                             (sewn/transcribe.h)
 *   voices.list | speak                                          (sewn/voices.h)
 *   calls.list{limit?} | calls.stats                             the calls ledger (sewn/calls.h)
 *
 * Replies are JSON frames: `<op>.result{…}`, `key.status{…}`, the turn's stream, or
 * `error{stage, message, status?}`. Every request names a `provider` or gets Mistral;
 * any other provider is answered with error{stage: "engine"} before a socket opens.
 * sewnd is the only process on MaryOS that talks to the network, and every call it
 * makes is a row in the calls ledger. */
#ifndef MARY_SEWN_SERVER_H
#define MARY_SEWN_SERVER_H

#include <stdbool.h>
#include <stddef.h>

#include "common/buf.h"
#include "sewn/calls.h"
#include "sewn/key.h"
#include "sewn/peer.h"
#include "sewn/retrieve.h"
#include "sewn/transport.h"

/* Checks a key with Mistral: 1 accepted, 0 refused, -errno unreachable. */
typedef int (*sewn_verify_fn)(const char *key, char *message, size_t cap, void *user);
/* GETs https://api.mistral.ai<path> with the key: at most `max` bytes of the body into `body`, the HTTP status into
 * *status. 0; -EMSGSIZE for a longer body; -EIO with `message` filled when Mistral could not be reached. */
typedef int (*sewn_get_fn)(const char *path, const char *key, mc_buf *body, size_t max, long *status, char *message,
                           size_t cap, void *user);

typedef struct sewn_service {
    sewn_key_store keys;
    const char *admin_group;
    sewn_group_fn in_group;
    sewn_verify_fn verify;      /* NULL when built without libcurl */
    void *verify_user;
    sewn_get_fn get;                    /* NULL when built without libcurl */
    void *get_user;
    sewn_post_stream_fn post_stream;    /* NULL when built without libcurl */
    void *post_stream_user;
    const sewn_ws_ops *ws;              /* NULL when built without libwebsockets */
    void *ws_user;
    int transcribe_wait_ms;             /* how long to wait for Voxtral's final transcript; 0: the default */
    size_t speech_line_max;             /* the longest speech event read; 0: SEWN_SPEECH_LINE_MAX (tests shrink it) */
    /* The Thread: retrieval for a turn and the auto-memory deposit. NULL: no retrieval,
     * no memory (the seams default to threadd's local socket; `*_user` is its path). */
    sewn_retrieve_fn retrieve;
    void *retrieve_user;
    sewn_deposit_fn deposit;
    void *deposit_user;
    sewn_calls calls;
} sewn_service;

void sewn_service_init(sewn_service *svc, const char *state_dir);
void sewn_service_free(sewn_service *svc);
/* Serves one connection to the end of its operation; the caller closes fd. 0, or
 * -errno when the connection failed before a reply could be sent. */
int sewn_serve_connection(sewn_service *svc, int fd, const sewn_peer *peer);
/* A listening unix socket at `path` with `mode`, replacing a stale socket (never
 * another kind of file). The fd, or -errno. */
int sewn_listen(const char *path, int mode);

/* Shared by the ops: error{stage, message[, status]} on fd. */
int sewn_send_error(int fd, const char *stage, const char *message, long status);
/* The stored key into `key`, or an error frame sent and -errno. The caller zeroes it. */
int sewn_stored_key(sewn_service *svc, int fd, char *key, size_t cap);

#endif
