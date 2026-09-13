#include "sewn/outbound.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "common/io.h"

struct counting {
    sewn_bytes_fn on_bytes;
    sewn_stop_fn should_stop;
    void *user;
    int64_t bytes_in;
};

static int count_bytes(const char *bytes, size_t len, long status, void *user) {
    struct counting *c = user;
    c->bytes_in += (int64_t)len;
    return c->on_bytes(bytes, len, status, c->user);
}

static bool count_stop(void *user) {
    struct counting *c = user;
    return c->should_stop ? c->should_stop(c->user) : false;
}

static void fill(sewn_call_row *row, const sewn_outbound *o, const char *path) {
    memset(row, 0, sizeof *row);
    snprintf(row->provider, sizeof row->provider, "%s", sewn_provider_name(o->provider));
    const char *host = sewn_provider_host(o->provider);
    snprintf(row->host, sizeof row->host, "%s", host ? host : "");
    snprintf(row->path, sizeof row->path, "%s", path ? path : "");
    snprintf(row->purpose, sizeof row->purpose, "%s", o->purpose ? o->purpose : "");
    snprintf(row->request_id, sizeof row->request_id, "%s", o->request_id ? o->request_id : "");
}

void sewn_record_call(sewn_service *svc, const sewn_outbound *o, const char *path, long status, int64_t ms, int64_t bytes_out,
                      int64_t bytes_in, const char *outcome) {
    sewn_call_row row;
    fill(&row, o, path);
    row.status = status;
    row.ms = ms;
    row.bytes_out = bytes_out;
    row.bytes_in = bytes_in;
    snprintf(row.outcome, sizeof row.outcome, "%s", outcome ? outcome : "");
    sewn_calls_record(&svc->calls, &row);
}

int sewn_post(sewn_service *svc, const sewn_outbound *o, const char *path, const char *key, const char *body, size_t body_len,
              sewn_bytes_fn on_bytes, sewn_stop_fn should_stop, void *user, long *status, char *message, size_t cap) {
    if (!svc->post_stream) {
        snprintf(message, cap, "sewnd was built without libcurl");
        return -ENOSYS;
    }
    struct counting c = { on_bytes, should_stop, user, 0 };
    int64_t started = mc_now_ms();
    *status = 0;
    int rc = svc->post_stream(path, key, body, body_len, count_bytes, count_stop, &c, status, message, cap, svc->post_stream_user);
    const char *outcome = rc == -ECANCELED ? "cancelled" : rc == SEWN_STREAM_STOPPED ? "stopped" : rc < 0 ? "failed"
                          : (*status >= 200 && *status <= 299) ? "ok" : "failed";
    sewn_record_call(svc, o, path, *status, mc_now_ms() - started, (int64_t)body_len, c.bytes_in, outcome);
    return rc;
}

int sewn_get(sewn_service *svc, const sewn_outbound *o, const char *path, const char *key, mc_buf *body, size_t max, long *status,
             char *message, size_t cap) {
    if (!svc->get) {
        snprintf(message, cap, "sewnd was built without libcurl");
        return -ENOSYS;
    }
    int64_t started = mc_now_ms();
    *status = 0;
    int rc = svc->get(path, key, body, max, status, message, cap, svc->get_user);
    sewn_record_call(svc, o, path, *status, mc_now_ms() - started, 0, (int64_t)body->len,
                     rc < 0 ? "failed" : (*status >= 200 && *status <= 299) ? "ok" : "failed");
    return rc;
}
