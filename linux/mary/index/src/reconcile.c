#include "index/reconcile.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/io.h"
#include "common/json.h"
#include "common/log.h"
#include "index/record.h"
#include "index/walk.h"

struct run {
    const ix_client *client;
    const char *root, *owner;
    ix_reconcile_stats *stats;
    struct json_object *page;
    char **todo;            /* paths threadd wants written */
    size_t n_todo, cap_todo;
    int error;
    char message[256];
};

static void want(struct run *r, const char *path) {
    if (r->n_todo == r->cap_todo) {
        r->cap_todo = r->cap_todo ? r->cap_todo * 2 : 64;
        r->todo = realloc(r->todo, r->cap_todo * sizeof *r->todo);
    }
    r->todo[r->n_todo++] = strdup(path);
}

static int flush(struct run *r, bool last) {
    struct json_object *report = NULL;
    int rc = ix_parity_report(r->client, &r->stats->run_id, r->page, last, &report, r->message, sizeof r->message);
    json_object_put(r->page);
    r->page = json_object_new_array();
    if (rc) {
        r->error = rc;
        return rc;
    }
    struct json_object *entries = mc_json_array(report, "entries");
    for (size_t i = 0; entries && i < json_object_array_length(entries); i++) {
        struct json_object *e = json_object_array_get_idx(entries, i);
        const char *status = mc_json_string(e, "status"), *path = mc_json_string(e, "path");
        if (!status || !path) continue;
        if (strcmp(status, "missing") == 0 || strcmp(status, "stale") == 0) want(r, path);
    }
    if (last) {
        int64_t v = 0;
        mc_json_int64(report, "seen", &v);
        mc_json_int64(report, "recorded", &v);
        r->stats->recorded = (long)v;
        v = 0;
        mc_json_int64(report, "missing", &v);
        r->stats->missing = (long)v;
        v = 0;
        mc_json_int64(report, "stale", &v);
        r->stats->stale = (long)v;
        v = 0;
        mc_json_int64(report, "orphaned", &v);
        r->stats->orphaned = (long)v;
    }
    json_object_put(report);
    return 0;
}

static int on_file(const char *path, int64_t size, int64_t mtime_ms, void *user) {
    struct run *r = user;
    char hash[65];
    if (ix_hash_file(path, hash) != 0) return 0;       /* gone or unreadable between the walk and now */
    r->stats->seen++;
    json_object_array_add(r->page, ix_parity_entry(path, size, mtime_ms, hash));
    if (json_object_array_length(r->page) >= IX_PARITY_PAGE) return flush(r, false) != 0;
    return 0;
}

int ix_reconcile(const ix_client *client, const char *root, const char *owner, ix_reconcile_stats *stats, char *message, size_t cap) {
    memset(stats, 0, sizeof *stats);
    int64_t started = mc_now_ms();
    struct run r = { .client = client, .root = root, .owner = owner, .stats = stats, .page = json_object_new_array() };
    long walked = ix_walk(root, on_file, &r);
    if (walked < 0) {
        json_object_put(r.page);
        snprintf(message, cap, "cannot walk %s: %s", root, strerror((int)-walked));
        return (int)walked;
    }
    if (!r.error) flush(&r, true);
    json_object_put(r.page);
    if (r.error) {
        for (size_t i = 0; i < r.n_todo; i++) free(r.todo[i]);
        free(r.todo);
        snprintf(message, cap, "%s", r.message);
        return r.error;
    }
    for (size_t i = 0; i < r.n_todo; i++) {
        int error = 0;
        struct json_object *record = ix_record(root, owner, r.todo[i], &error);
        if (!record) {
            stats->failed++;
        } else {
            char why[256] = "";
            if (ix_deposit(client, record, NULL, 0, why, sizeof why) == 0) stats->deposited++;
            else {
                stats->failed++;
                mc_log(MC_LOG_WARNING, "reconcile: %s was not recorded: %s", r.todo[i], why);
            }
            json_object_put(record);
        }
        free(r.todo[i]);
    }
    free(r.todo);
    stats->ms = mc_now_ms() - started;
    return 0;
}
