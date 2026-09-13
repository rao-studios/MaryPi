#include "computer-use/invoke.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/io.h"
#include "common/json.h"

struct call {
    char id[48];
    int64_t deadline_ms;
    mcu_result_fn done;
    void *user;
};

struct mcu_pipes {
    mcu_send_fn send;
    void *send_user;
    pthread_mutex_t lock;
    struct call *calls;
    size_t count, cap;
    unsigned long next;
};

mcu_pipes *mcu_pipes_new(mcu_send_fn send, void *send_user) {
    mcu_pipes *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->send = send;
    p->send_user = send_user;
    pthread_mutex_init(&p->lock, NULL);
    return p;
}

/* Takes the calls out under the lock and finishes them outside it. */
static void finish_all(mcu_pipes *p, const char *why, bool only_expired, int64_t now_ms) {
    pthread_mutex_lock(&p->lock);
    struct call *ended = p->count ? malloc(p->count * sizeof *ended) : NULL;
    size_t n = 0, kept = 0;
    for (size_t i = 0; i < p->count; i++) {
        if (!only_expired || p->calls[i].deadline_ms <= now_ms) {
            if (ended) ended[n++] = p->calls[i];
        } else {
            p->calls[kept++] = p->calls[i];
        }
    }
    p->count = kept;
    pthread_mutex_unlock(&p->lock);
    for (size_t i = 0; i < n; i++) {
        mcu_result r = { .call_id = ended[i].id, .ok = false, .error = why };
        if (ended[i].done) ended[i].done(&r, ended[i].user);
    }
    free(ended);
}

void mcu_pipes_free(mcu_pipes *p) {
    if (!p) return;
    finish_all(p, "disconnected", false, 0);
    pthread_mutex_destroy(&p->lock);
    free(p->calls);
    free(p);
}

int mcu_invoke(mcu_pipes *p, const char *app, const char *skill, struct json_object *args, int timeout_ms,
               mcu_result_fn done, void *user, char *call_id, size_t cap) {
    if (!app || !skill) return -EINVAL;
    struct call call = { .done = done, .user = user };
    pthread_mutex_lock(&p->lock);
    snprintf(call.id, sizeof call.id, "call-%lu-%lld", ++p->next, (long long)mc_wall_ms());
    pthread_mutex_unlock(&p->lock);
    call.deadline_ms = mc_now_ms() + (timeout_ms > 0 ? timeout_ms : 10000);

    struct json_object *msg = json_object_new_object();
    json_object_object_add(msg, "type", json_object_new_string("skill.invoke"));
    json_object_object_add(msg, "call_id", json_object_new_string(call.id));
    json_object_object_add(msg, "app", json_object_new_string(app));
    json_object_object_add(msg, "skill", json_object_new_string(skill));
    json_object_object_add(msg, "args", args ? json_object_get(args) : json_object_new_object());

    /* Registered before sending: a result may arrive before send returns. */
    pthread_mutex_lock(&p->lock);
    if (p->count == p->cap) {
        size_t grown_cap = p->cap ? p->cap * 2 : 8;
        struct call *grown = realloc(p->calls, grown_cap * sizeof *grown);
        if (!grown) {
            pthread_mutex_unlock(&p->lock);
            json_object_put(msg);
            return -ENOMEM;
        }
        p->calls = grown;
        p->cap = grown_cap;
    }
    p->calls[p->count++] = call;
    pthread_mutex_unlock(&p->lock);

    int rc = p->send(msg, p->send_user);
    json_object_put(msg);
    if (rc < 0) {
        pthread_mutex_lock(&p->lock);
        for (size_t i = 0; i < p->count; i++) {
            if (strcmp(p->calls[i].id, call.id) == 0) {
                p->calls[i] = p->calls[--p->count];
                break;
            }
        }
        pthread_mutex_unlock(&p->lock);
        return rc;
    }
    if (call_id && cap) snprintf(call_id, cap, "%s", call.id);
    return 0;
}

int mcu_pipes_on_message(mcu_pipes *p, struct json_object *message) {
    const char *type = mc_json_type(message);
    if (!type || strcmp(type, "skill.result") != 0) return 0;
    const char *id = mc_json_string(message, "call_id");
    struct call found;
    bool matched = false;
    pthread_mutex_lock(&p->lock);
    for (size_t i = 0; id && i < p->count; i++) {
        if (strcmp(p->calls[i].id, id) == 0) {
            found = p->calls[i];
            p->calls[i] = p->calls[--p->count];
            matched = true;
            break;
        }
    }
    pthread_mutex_unlock(&p->lock);
    if (!matched) return -ENOENT;
    bool ok = false;
    mc_json_bool(message, "ok", &ok);
    const char *error = mc_json_string(message, "error");
    struct json_object *result;
    mcu_result r = { .call_id = found.id, .ok = ok, .error = ok ? NULL : (error ? error : "failed") };
    r.result = ok && json_object_object_get_ex(message, "result", &result) ? result : NULL;
    if (found.done) found.done(&r, found.user);
    return 1;
}

void mcu_pipes_tick(mcu_pipes *p, int64_t now_ms) { finish_all(p, "timeout", true, now_ms); }
void mcu_pipes_disconnect(mcu_pipes *p) { finish_all(p, "disconnected", false, 0); }

size_t mcu_pipes_pending(const mcu_pipes *p) {
    pthread_mutex_lock((pthread_mutex_t *)&p->lock);
    size_t n = p->count;
    pthread_mutex_unlock((pthread_mutex_t *)&p->lock);
    return n;
}

int mcu_app_state(mcu_pipes *p, const char *app, mcu_result_fn done, void *user) { return -ENOSYS; }
