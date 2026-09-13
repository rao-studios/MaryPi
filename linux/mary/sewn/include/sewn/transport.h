/* How sewnd reaches Mistral, as one function it is handed: POST a JSON body to a
 * path on api.mistral.ai and stream the answer back. Production passes
 * sewn_http_post_stream (libcurl); tests pass a script. Keeping the seam here
 * means no test host or plain-HTTP switch ever exists in the daemon. */
#ifndef MARY_SEWN_TRANSPORT_H
#define MARY_SEWN_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>

/* Response bytes as they arrive, with the HTTP status once it is known (0 before).
 * Return nonzero to end the transfer: the consumer has what it needs. */
typedef int (*sewn_bytes_fn)(const char *bytes, size_t len, long status, void *user);
/* Polled while the transfer runs; true aborts it (the client cancelled). */
typedef bool (*sewn_stop_fn)(void *user);

#define SEWN_STREAM_STOPPED 1

/* 0 when the answer ended, SEWN_STREAM_STOPPED when on_bytes ended it,
 * -ECANCELED when should_stop did, or -EIO with `message` filled. *status is the
 * HTTP status (0 when none arrived). The key is only put in a header. */
typedef int (*sewn_post_stream_fn)(const char *path, const char *key, const char *body, size_t body_len,
                                   sewn_bytes_fn on_bytes, sewn_stop_fn should_stop, void *user,
                                   long *status, char *message, size_t cap, void *transport_user);

/* The realtime side: a WebSocket to Voxtral Realtime. Callbacks run on the
 * transport's own thread; on_closed runs at most once; none run after close()
 * has returned. send() may be called from any thread and queues the message. */
typedef void (*sewn_ws_message_fn)(const char *text, size_t len, void *user);
typedef void (*sewn_ws_closed_fn)(const char *reason, void *user);
typedef struct sewn_ws sewn_ws;

typedef struct sewn_ws_ops {
    /* wss://api.mistral.ai<path> with the key as a bearer header. NULL with
     * `message` filled when the connection cannot even start. */
    sewn_ws *(*open)(const char *path, const char *key, sewn_ws_message_fn on_message, sewn_ws_closed_fn on_closed,
                     void *user, char *message, size_t cap, void *transport_user);
    /* A text message. 0, or -errno once the session is closed. */
    int (*send)(sewn_ws *ws, const char *text, size_t len);
    /* Closes the session and frees it. */
    void (*close)(sewn_ws *ws);
} sewn_ws_ops;

#endif
