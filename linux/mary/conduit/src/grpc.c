#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "conduit/grpc.h"

#include <errno.h>
#include <nghttp2/nghttp2.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/buf.h"
#include "common/io.h"

#define PREFIX 5
#define NV(name, value) \
    { (uint8_t *)(name), (uint8_t *)(value), sizeof(name) - 1, strlen(value), NGHTTP2_NV_FLAG_NONE }

/* MARK: - Shared */

/* grpc-message is percent-encoded: everything outside printable ASCII, and '%'. */
static void percent_encode(const char *in, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        if (*p >= 0x20 && *p <= 0x7E && *p != '%') {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        }
    }
    out[o] = 0;
}

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void percent_decode(const uint8_t *in, size_t len, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < cap; i++) {
        int hi, lo;
        if (in[i] == '%' && i + 2 < len && (hi = hex_value(in[i + 1])) >= 0 && (lo = hex_value(in[i + 2])) >= 0) {
            out[o++] = (char)(hi << 4 | lo);
            i += 2;
        } else {
            out[o++] = (char)in[i];
        }
    }
    out[o] = 0;
}

static uint8_t *prefixed(const uint8_t *message, size_t len) {
    uint8_t *out = malloc(PREFIX + len);
    if (!out) return NULL;
    out[0] = 0;
    out[1] = (uint8_t)(len >> 24);
    out[2] = (uint8_t)(len >> 16);
    out[3] = (uint8_t)(len >> 8);
    out[4] = (uint8_t)len;
    if (len) memcpy(out + PREFIX, message, len);
    return out;
}

/* A whole prefixed message: its length, or -1 when `bytes` is not exactly one. */
static long unframe(const mc_buf *bytes, int *compressed) {
    if (bytes->len < PREFIX) return -1;
    const uint8_t *h = bytes->data;
    uint32_t len = (uint32_t)h[1] << 24 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 8 | h[4];
    *compressed = h[0];
    return (size_t)len + PREFIX == bytes->len ? (long)len : -1;
}

static int flush(nghttp2_session *session, int fd) {
    for (;;) {
        const uint8_t *data;
        ssize_t n = nghttp2_session_mem_send(session, &data);
        if (n < 0) return -EPROTO;
        if (n == 0) return 0;
        int rc = mc_write_all(fd, data, (size_t)n);
        if (rc < 0) return rc;
    }
}

/* MARK: - Server */

struct stream {
    char path[256];
    bool grpc;
    bool too_big;
    mc_buf body;
    uint8_t *out;
    size_t out_len, out_off;
    struct stream *next;
};

struct server {
    nghttp2_session *session;
    const conduit_route *routes;
    size_t route_count;
    void *user;
    struct stream *streams;
};

static void free_stream(struct server *s, struct stream *st) {
    for (struct stream **p = &s->streams; *p; p = &(*p)->next) {
        if (*p == st) {
            *p = st->next;
            break;
        }
    }
    mc_buf_free(&st->body);
    free(st->out);
    free(st);
}

static int on_begin_headers(nghttp2_session *session, const nghttp2_frame *frame, void *user) {
    struct server *s = user;
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) return 0;
    struct stream *st = calloc(1, sizeof *st);
    if (!st) return NGHTTP2_ERR_CALLBACK_FAILURE;
    st->next = s->streams;
    s->streams = st;
    nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, st);
    return 0;
}

static int on_header(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen,
                     const uint8_t *value, size_t valuelen, uint8_t flags, void *user) {
    struct stream *st = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    if (!st) return 0;
    if (namelen == 5 && memcmp(name, ":path", 5) == 0) {
        size_t n = valuelen < sizeof st->path - 1 ? valuelen : sizeof st->path - 1;
        memcpy(st->path, value, n);
        st->path[n] = 0;
    } else if (namelen == 12 && memcmp(name, "content-type", 12) == 0) {
        st->grpc = valuelen >= 16 && memcmp(value, "application/grpc", 16) == 0;
    }
    return 0;
}

static int on_data_chunk(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user) {
    struct stream *st = nghttp2_session_get_stream_user_data(session, stream_id);
    if (!st || st->too_big) return 0;
    if (st->body.len + len > CONDUIT_MESSAGE_MAX + PREFIX) {
        st->too_big = true;
        mc_buf_free(&st->body);
        return 0;
    }
    return mc_buf_append(&st->body, data, len) < 0 ? NGHTTP2_ERR_CALLBACK_FAILURE : 0;
}

static ssize_t read_response(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length,
                             uint32_t *data_flags, nghttp2_data_source *source, void *user) {
    struct stream *st = source->ptr;
    size_t left = st->out_len - st->out_off, n = left < length ? left : length;
    memcpy(buf, st->out + st->out_off, n);
    st->out_off += n;
    if (st->out_off == st->out_len) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF | NGHTTP2_DATA_FLAG_NO_END_STREAM;
        nghttp2_nv trailers[] = { NV("grpc-status", "0") };
        if (nghttp2_submit_trailer(session, stream_id, trailers, 1) != 0) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    return (ssize_t)n;
}

static void answer(struct server *s, int32_t stream_id, struct stream *st) {
    conduit_reply reply = { .status = CONDUIT_OK };
    int compressed = 0;
    long len = st->too_big ? -1 : unframe(&st->body, &compressed);
    const conduit_route *route = NULL;
    for (size_t i = 0; i < s->route_count; i++)
        if (strcmp(s->routes[i].path, st->path) == 0) route = &s->routes[i];

    if (st->too_big) {
        reply.status = CONDUIT_RESOURCE_EXHAUSTED;
        snprintf(reply.message, sizeof reply.message, "the request is larger than 4 MiB");
    } else if (!st->grpc) {
        reply.status = CONDUIT_INTERNAL;
        snprintf(reply.message, sizeof reply.message, "content-type must be application/grpc");
    } else if (!route) {
        reply.status = CONDUIT_UNIMPLEMENTED;
        snprintf(reply.message, sizeof reply.message, "unknown method %.200s", st->path);
    } else if (len < 0) {
        reply.status = CONDUIT_INTERNAL;
        snprintf(reply.message, sizeof reply.message, "the request is not one length-prefixed message");
    } else if (compressed) {
        reply.status = CONDUIT_UNIMPLEMENTED;
        snprintf(reply.message, sizeof reply.message, "compressed messages are not supported");
    } else {
        route->handle(st->body.data + PREFIX, (size_t)len, &reply, s->user);
        if (reply.status == CONDUIT_OK && reply.body_len > CONDUIT_MESSAGE_MAX) {
            reply.status = CONDUIT_RESOURCE_EXHAUSTED;
            snprintf(reply.message, sizeof reply.message, "the response is larger than 4 MiB");
        }
    }

    if (reply.status == CONDUIT_OK) {
        st->out = prefixed(reply.body, reply.body_len);
        st->out_len = PREFIX + reply.body_len;
        free(reply.body);
        if (st->out) {
            nghttp2_nv headers[] = { NV(":status", "200"), NV("content-type", "application/grpc") };
            nghttp2_data_provider provider = { .source.ptr = st, .read_callback = read_response };
            nghttp2_submit_response(s->session, stream_id, headers, 2, &provider);
            return;
        }
        reply.status = CONDUIT_RESOURCE_EXHAUSTED;
        snprintf(reply.message, sizeof reply.message, "out of memory");
    }
    free(reply.body);
    char status[12], message[768];
    snprintf(status, sizeof status, "%d", reply.status);
    percent_encode(reply.message, message, sizeof message);
    nghttp2_nv trailers_only[] = {
        NV(":status", "200"), NV("content-type", "application/grpc"), NV("grpc-status", status), NV("grpc-message", message),
    };
    nghttp2_submit_response(s->session, stream_id, trailers_only, 4, NULL);
}

static int on_frame(nghttp2_session *session, const nghttp2_frame *frame, void *user) {
    if ((frame->hd.type == NGHTTP2_DATA || frame->hd.type == NGHTTP2_HEADERS) && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        struct stream *st = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
        if (st) answer(user, frame->hd.stream_id, st);
    }
    return 0;
}

static int on_stream_close(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user) {
    struct stream *st = nghttp2_session_get_stream_user_data(session, stream_id);
    if (st) free_stream(user, st);
    return 0;
}

int conduit_serve(int fd, const conduit_route *routes, size_t route_count, void *user) {
    struct server s = { .routes = routes, .route_count = route_count, .user = user };
    nghttp2_session_callbacks *callbacks;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) return -ENOMEM;
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close);
    int rc = nghttp2_session_server_new(&s.session, callbacks, &s);
    nghttp2_session_callbacks_del(callbacks);
    if (rc != 0) return -ENOMEM;
    nghttp2_settings_entry settings[] = { { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 32 } };
    nghttp2_submit_settings(s.session, NGHTTP2_FLAG_NONE, settings, 1);

    uint8_t buf[16384];
    rc = 0;
    for (;;) {
        if ((rc = flush(s.session, fd)) < 0) break;
        if (!nghttp2_session_want_read(s.session) && !nghttp2_session_want_write(s.session)) break;
        ssize_t got = read(fd, buf, sizeof buf);
        if (got < 0) {
            if (errno == EINTR) continue;
            rc = -errno;
            break;
        }
        if (got == 0) break;
        if (nghttp2_session_mem_recv(s.session, buf, (size_t)got) < 0) {
            rc = -EPROTO;
            break;
        }
    }
    nghttp2_session_del(s.session);
    while (s.streams) free_stream(&s, s.streams);
    return rc;
}

/* MARK: - Client */

struct call {
    uint8_t *out;
    size_t out_len, out_off;
    mc_buf body;
    bool too_big;
    bool closed;
    bool status_seen;
    int status;
    int http_status;
    char message[256];
};

static ssize_t read_request(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length,
                            uint32_t *data_flags, nghttp2_data_source *source, void *user) {
    struct call *c = source->ptr;
    size_t left = c->out_len - c->out_off, n = left < length ? left : length;
    memcpy(buf, c->out + c->out_off, n);
    c->out_off += n;
    if (c->out_off == c->out_len) *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return (ssize_t)n;
}

static int on_response_header(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen,
                              const uint8_t *value, size_t valuelen, uint8_t flags, void *user) {
    struct call *c = user;
    char text[32];
    if (namelen == 7 && memcmp(name, ":status", 7) == 0) {
        snprintf(text, sizeof text, "%.*s", (int)(valuelen < 31 ? valuelen : 31), value);
        c->http_status = atoi(text);
    } else if (namelen == 11 && memcmp(name, "grpc-status", 11) == 0) {
        snprintf(text, sizeof text, "%.*s", (int)(valuelen < 31 ? valuelen : 31), value);
        c->status = atoi(text);
        c->status_seen = true;
    } else if (namelen == 12 && memcmp(name, "grpc-message", 12) == 0) {
        percent_decode(value, valuelen, c->message, sizeof c->message);
    }
    return 0;
}

static int on_response_data(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user) {
    struct call *c = user;
    if (c->too_big) return 0;
    if (c->body.len + len > CONDUIT_MESSAGE_MAX + PREFIX) {
        c->too_big = true;
        nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_CANCEL);
        return 0;
    }
    return mc_buf_append(&c->body, data, len) < 0 ? NGHTTP2_ERR_CALLBACK_FAILURE : 0;
}

static int on_response_close(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user) {
    ((struct call *)user)->closed = true;
    return 0;
}

int conduit_call(int fd, const char *path, const uint8_t *request, size_t len, int timeout_ms, conduit_result *result) {
    memset(result, 0, sizeof *result);
    if (len > CONDUIT_MESSAGE_MAX) return -EMSGSIZE;
    struct call c = { .out = prefixed(request, len), .out_len = PREFIX + len };
    if (!c.out) return -ENOMEM;
    nghttp2_session_callbacks *callbacks;
    nghttp2_session *session;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        free(c.out);
        return -ENOMEM;
    }
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_response_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_response_data);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_response_close);
    int rc = nghttp2_session_client_new(&session, callbacks, &c);
    nghttp2_session_callbacks_del(callbacks);
    if (rc != 0) {
        free(c.out);
        return -ENOMEM;
    }
    nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, NULL, 0);
    nghttp2_nv headers[] = {
        NV(":method", "POST"), NV(":scheme", "http"), NV(":path", path), NV(":authority", "localhost"),
        NV("content-type", "application/grpc"), NV("te", "trailers"), NV("user-agent", "conduit-c/" MARY_VERSION),
    };
    nghttp2_data_provider provider = { .source.ptr = &c, .read_callback = read_request };
    rc = nghttp2_submit_request(session, NULL, headers, sizeof headers / sizeof headers[0], &provider, NULL) < 0 ? -EPROTO : 0;

    int64_t deadline = mc_now_ms() + (timeout_ms > 0 ? timeout_ms : 30000);
    uint8_t buf[16384];
    while (rc == 0 && !c.closed) {
        if ((rc = flush(session, fd)) < 0) break;
        int64_t left = deadline - mc_now_ms();
        if (left <= 0) {
            rc = -ETIMEDOUT;
            break;
        }
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int ready = poll(&p, 1, (int)left);
        if (ready < 0) {
            if (errno == EINTR) continue;
            rc = -errno;
            break;
        }
        if (ready == 0) continue;
        ssize_t got = read(fd, buf, sizeof buf);
        if (got < 0) {
            if (errno == EINTR) continue;
            rc = -errno;
            break;
        }
        if (got == 0) {
            rc = -ECONNRESET;
            break;
        }
        if (nghttp2_session_mem_recv(session, buf, (size_t)got) < 0) rc = -EPROTO;
    }
    if (rc == 0) {
        int compressed = 0;
        long message_len = c.body.len ? unframe(&c.body, &compressed) : -1;
        result->status = c.status_seen ? c.status : CONDUIT_UNKNOWN;
        snprintf(result->message, sizeof result->message, "%s", c.message);
        if (c.too_big) {
            result->status = CONDUIT_RESOURCE_EXHAUSTED;
            snprintf(result->message, sizeof result->message, "the response is larger than 4 MiB");
        } else if (!c.status_seen) {
            snprintf(result->message, sizeof result->message, "the response carried no grpc-status (HTTP %d)", c.http_status);
        } else if (result->status == CONDUIT_OK && c.body.len && (message_len < 0 || compressed)) {
            result->status = CONDUIT_INTERNAL;
            snprintf(result->message, sizeof result->message, "the response is not one uncompressed message");
        } else if (result->status == CONDUIT_OK && message_len >= 0) {
            result->body = malloc(message_len ? (size_t)message_len : 1);
            if (!result->body) {
                rc = -ENOMEM;
            } else {
                memcpy(result->body, c.body.data + PREFIX, (size_t)message_len);
                result->body_len = (size_t)message_len;
            }
        }
    }
    nghttp2_session_del(session);
    mc_buf_free(&c.body);
    free(c.out);
    return rc;
}

int conduit_connect_unix(const char *path) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    size_t n = strlen(path);
    if (n >= sizeof addr.sun_path) return -ENAMETOOLONG;
    memcpy(addr.sun_path, path, n + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -errno;
    mc_set_cloexec(fd);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}
