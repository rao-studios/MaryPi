#include "sewn/calls.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/io.h"
#include "common/json.h"

void sewn_calls_init(sewn_calls *c, const char *state_dir) {
    memset(c, 0, sizeof *c);
    pthread_mutex_init(&c->lock, NULL);
    c->ready = true;
    c->next_id = 1;
    if (state_dir && *state_dir) snprintf(c->path, sizeof c->path, "%s/calls.jsonl", state_dir);
}

void sewn_calls_free(sewn_calls *c) {
    if (c->ready) pthread_mutex_destroy(&c->lock);
    c->ready = false;
}

static struct json_object *row_json(const sewn_call_row *r) {
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "id", json_object_new_int64(r->id));
    json_object_object_add(o, "at_ms", json_object_new_int64(r->at_ms));
    json_object_object_add(o, "provider", json_object_new_string(r->provider));
    json_object_object_add(o, "host", json_object_new_string(r->host));
    json_object_object_add(o, "path", json_object_new_string(r->path));
    json_object_object_add(o, "purpose", json_object_new_string(r->purpose));
    if (r->request_id[0]) json_object_object_add(o, "request_id", json_object_new_string(r->request_id));
    json_object_object_add(o, "status", json_object_new_int64(r->status));
    json_object_object_add(o, "ms", json_object_new_int64(r->ms));
    json_object_object_add(o, "bytes_out", json_object_new_int64(r->bytes_out));
    json_object_object_add(o, "bytes_in", json_object_new_int64(r->bytes_in));
    json_object_object_add(o, "outcome", json_object_new_string(r->outcome));
    return o;
}

/* Appends one line; a file past SEWN_CALLS_FILE_MAX is rotated to calls.jsonl.1 first. */
static void append_line(sewn_calls *c, struct json_object *o) {
    if (!c->path[0]) return;
    struct stat st;
    if (stat(c->path, &st) == 0 && st.st_size >= (off_t)SEWN_CALLS_FILE_MAX) {
        char old[280];
        snprintf(old, sizeof old, "%s.1", c->path);
        rename(c->path, old);
    }
    int fd = open(c->path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    size_t n = 0;
    const char *text = mc_json_compact(o, &n);
    mc_write_all(fd, text, n);
    mc_write_all(fd, "\n", 1);
    close(fd);
}

int64_t sewn_calls_record(sewn_calls *c, sewn_call_row *row) {
    if (!c->ready) return 0;
    pthread_mutex_lock(&c->lock);
    row->id = c->next_id++;
    if (!row->at_ms) row->at_ms = mc_wall_ms();
    if (!row->outcome[0]) snprintf(row->outcome, sizeof row->outcome, "%s", row->status >= 200 && row->status <= 299 ? "ok" : "failed");
    size_t slot = (c->head + c->count) % SEWN_CALLS_RING;
    if (c->count == SEWN_CALLS_RING) c->head = (c->head + 1) % SEWN_CALLS_RING;
    else c->count++;
    c->ring[slot] = *row;
    struct json_object *o = row_json(row);
    append_line(c, o);
    json_object_put(o);
    int64_t id = row->id;
    pthread_mutex_unlock(&c->lock);
    return id;
}

struct json_object *sewn_calls_list(sewn_calls *c, int limit) {
    struct json_object *arr = json_object_new_array();
    if (!c->ready) return arr;
    pthread_mutex_lock(&c->lock);
    size_t n = c->count;
    if (limit > 0 && (size_t)limit < n) n = (size_t)limit;
    for (size_t i = 0; i < n; i++) {
        size_t slot = (c->head + c->count - 1 - i) % SEWN_CALLS_RING;
        json_object_array_add(arr, row_json(&c->ring[slot]));
    }
    pthread_mutex_unlock(&c->lock);
    return arr;
}

static void bump(struct json_object *counts, const char *key) {
    struct json_object *v = NULL;
    int64_t n = json_object_object_get_ex(counts, key, &v) ? json_object_get_int64(v) : 0;
    json_object_object_add(counts, key, json_object_new_int64(n + 1));
}

struct json_object *sewn_calls_stats(sewn_calls *c) {
    struct json_object *o = json_object_new_object(), *by_purpose = json_object_new_object(), *by_provider = json_object_new_object();
    int64_t failed = 0, out = 0, in = 0, first = 0, last = 0, ms = 0;
    if (c->ready) pthread_mutex_lock(&c->lock);
    for (size_t i = 0; i < c->count; i++) {
        const sewn_call_row *r = &c->ring[(c->head + i) % SEWN_CALLS_RING];
        bump(by_purpose, r->purpose);
        bump(by_provider, r->provider);
        if (strcmp(r->outcome, "ok") != 0) failed++;
        out += r->bytes_out;
        in += r->bytes_in;
        ms += r->ms;
        if (!first || r->at_ms < first) first = r->at_ms;
        if (r->at_ms > last) last = r->at_ms;
    }
    json_object_object_add(o, "count", json_object_new_int64((int64_t)c->count));
    if (c->ready) pthread_mutex_unlock(&c->lock);
    json_object_object_add(o, "by_purpose", by_purpose);
    json_object_object_add(o, "by_provider", by_provider);
    json_object_object_add(o, "failed", json_object_new_int64(failed));
    json_object_object_add(o, "bytes_out", json_object_new_int64(out));
    json_object_object_add(o, "bytes_in", json_object_new_int64(in));
    json_object_object_add(o, "ms", json_object_new_int64(ms));
    json_object_object_add(o, "first_ms", json_object_new_int64(first));
    json_object_object_add(o, "last_ms", json_object_new_int64(last));
    return o;
}
