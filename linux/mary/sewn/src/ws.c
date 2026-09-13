#include "sewn/ws.h"

#ifdef HAVE_LWS
#include <errno.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/buf.h"
#include "common/io.h"
#include "common/log.h"
#include "common/secure.h"
#include "sewn/key.h"
#include "sewn/mistral.h"

#define CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"   /* ca-certificates, in base.list */
#define CLOSE_GRACE_MS 2000
#define PROTOCOL "mary-voxtral"

struct outgoing {
    char *text;
    size_t len;
};

struct sewn_ws {
    struct lws_context *context;
    struct lws *wsi;
    pthread_t thread;
    pthread_mutex_t lock;
    struct outgoing *queue;
    size_t head, count, cap;
    mc_buf inbox;
    char auth[SEWN_KEY_MAX + 16];
    atomic_bool established, closing, closed;
    atomic_llong closing_since;
    sewn_ws_message_fn on_message;
    sewn_ws_closed_fn on_closed;
    void *user;
};

static void report_closed(struct sewn_ws *ws, const char *reason) {
    if (atomic_exchange(&ws->closed, true)) return;
    if (ws->on_closed) ws->on_closed(reason, ws->user);
}

static int voxtral_callback(struct lws *wsi, enum lws_callback_reasons reason, void *per_session, void *in, size_t len) {
    struct lws_context *context = lws_get_context(wsi);
    struct sewn_ws *ws = context ? lws_context_user(context) : NULL;
    if (!ws) return 0;
    switch (reason) {
    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
        unsigned char **p = (unsigned char **)in, *end = *p + len;
        int failed = lws_add_http_header_by_name(wsi, (const unsigned char *)"authorization:",
                                                 (const unsigned char *)ws->auth, (int)strlen(ws->auth), p, end);
        mc_secure_zero(ws->auth, sizeof ws->auth);
        return failed ? -1 : 0;
    }
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        atomic_store(&ws->established, true);
        lws_callback_on_writable(wsi);
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (lws_frame_is_binary(wsi)) break;
        if (mc_buf_append(&ws->inbox, in, len) < 0) return -1;
        if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
            if (ws->on_message) ws->on_message((const char *)ws->inbox.data, ws->inbox.len, ws->user);
            mc_buf_clear(&ws->inbox);
        }
        break;
    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        struct outgoing out = { 0 };
        pthread_mutex_lock(&ws->lock);
        if (ws->count) {
            out = ws->queue[ws->head++];
            if (--ws->count == 0) ws->head = 0;
        }
        bool more = ws->count > 0;
        pthread_mutex_unlock(&ws->lock);
        if (out.text) {
            unsigned char *frame = malloc(LWS_PRE + out.len);
            int wrote = -1;
            if (frame) {
                memcpy(frame + LWS_PRE, out.text, out.len);
                wrote = lws_write(wsi, frame + LWS_PRE, out.len, LWS_WRITE_TEXT);
            }
            free(frame);
            free(out.text);
            if (wrote < (int)out.len) return -1;
        }
        if (more) {
            lws_callback_on_writable(wsi);
        } else if (atomic_load(&ws->closing)) {
            lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
            return -1;
        }
        break;
    }
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
        if (ws->wsi && atomic_load(&ws->established)) lws_callback_on_writable(ws->wsi);
        break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
        char why[200];
        snprintf(why, sizeof why, "could not reach Voxtral: %s", in ? (const char *)in : "connection failed");
        ws->wsi = NULL;
        report_closed(ws, why);
        break;
    }
    case LWS_CALLBACK_CLIENT_CLOSED:
        ws->wsi = NULL;
        report_closed(ws, "Voxtral closed the session");
        break;
    default:
        break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    { .name = PROTOCOL, .callback = voxtral_callback },
    LWS_PROTOCOL_LIST_TERM,
};

static void log_line(int level, const char *line) {
    char trimmed[256];
    snprintf(trimmed, sizeof trimmed, "%s", line);
    size_t n = strlen(trimmed);
    while (n && (trimmed[n - 1] == '\n' || trimmed[n - 1] == '\r')) trimmed[--n] = 0;
    mc_log(level == LLL_ERR ? MC_LOG_ERROR : MC_LOG_WARNING, "libwebsockets: %s", trimmed);
}

static void quiet_libwebsockets(void) { lws_set_log_level(LLL_ERR | LLL_WARN, log_line); }

static void *service(void *arg) {
    struct sewn_ws *ws = arg;
    while (!atomic_load(&ws->closed)) {
        lws_service(ws->context, 100);
        if (atomic_load(&ws->closing) && mc_now_ms() - atomic_load(&ws->closing_since) > CLOSE_GRACE_MS) break;
    }
    return NULL;
}

static void free_session(struct sewn_ws *ws) {
    for (size_t i = 0; i < ws->count; i++) free(ws->queue[ws->head + i].text);
    free(ws->queue);
    mc_buf_free(&ws->inbox);
    mc_secure_zero(ws->auth, sizeof ws->auth);
    pthread_mutex_destroy(&ws->lock);
    free(ws);
}

static sewn_ws *voxtral_ws_open(const char *path, const char *key, sewn_ws_message_fn on_message, sewn_ws_closed_fn on_closed,
                         void *user, char *message, size_t cap, void *transport_user) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, quiet_libwebsockets);
    struct sewn_ws *ws = calloc(1, sizeof *ws);
    if (!ws) {
        snprintf(message, cap, "out of memory");
        return NULL;
    }
    pthread_mutex_init(&ws->lock, NULL);
    snprintf(ws->auth, sizeof ws->auth, "Bearer %s", key);
    ws->on_message = on_message;
    ws->on_closed = on_closed;
    ws->user = user;

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.client_ssl_ca_filepath = CA_BUNDLE;
    info.user = ws;
    ws->context = lws_create_context(&info);
    if (!ws->context) {
        snprintf(message, cap, "libwebsockets did not start");
        free_session(ws);
        return NULL;
    }

    struct lws_client_connect_info cc;
    memset(&cc, 0, sizeof cc);
    cc.context = ws->context;
    cc.address = SEWN_MISTRAL_HOST;
    cc.port = 443;
    cc.ssl_connection = LCCSCF_USE_SSL;
    cc.path = path;
    cc.host = SEWN_MISTRAL_HOST;
    cc.origin = SEWN_MISTRAL_HOST;
    cc.local_protocol_name = PROTOCOL;
    cc.pwsi = &ws->wsi;
    if (!lws_client_connect_via_info(&cc) && !atomic_load(&ws->closed)) {
        snprintf(message, cap, "could not start a connection to Voxtral");
        lws_context_destroy(ws->context);
        free_session(ws);
        return NULL;
    }
    if (pthread_create(&ws->thread, NULL, service, ws) != 0) {
        snprintf(message, cap, "could not start the Voxtral thread");
        lws_context_destroy(ws->context);
        free_session(ws);
        return NULL;
    }
    return ws;
}

static int voxtral_ws_send(sewn_ws *ws, const char *text, size_t len) {
    if (atomic_load(&ws->closed)) return -EPIPE;
    char *copy = malloc(len ? len : 1);
    if (!copy) return -ENOMEM;
    memcpy(copy, text, len);
    pthread_mutex_lock(&ws->lock);
    if (ws->head + ws->count == ws->cap) {
        if (ws->head) {
            memmove(ws->queue, ws->queue + ws->head, ws->count * sizeof *ws->queue);
            ws->head = 0;
        } else {
            size_t cap = ws->cap ? ws->cap * 2 : 32;
            struct outgoing *grown = realloc(ws->queue, cap * sizeof *grown);
            if (!grown) {
                pthread_mutex_unlock(&ws->lock);
                free(copy);
                return -ENOMEM;
            }
            ws->queue = grown;
            ws->cap = cap;
        }
    }
    ws->queue[ws->head + ws->count++] = (struct outgoing){ copy, len };
    pthread_mutex_unlock(&ws->lock);
    lws_cancel_service(ws->context);
    return 0;
}

static void voxtral_ws_close(sewn_ws *ws) {
    atomic_store(&ws->closing_since, mc_now_ms());
    atomic_store(&ws->closing, true);
    lws_cancel_service(ws->context);
    pthread_join(ws->thread, NULL);
    ws->on_message = NULL;
    ws->on_closed = NULL;
    lws_context_destroy(ws->context);
    free_session(ws);
}

const sewn_ws_ops sewn_lws_ops = { voxtral_ws_open, voxtral_ws_send, voxtral_ws_close };
#else
typedef int sewn_ws_without_libwebsockets;
#endif
