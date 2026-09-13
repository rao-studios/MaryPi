#include "runtime/queue.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include "common/io.h"
#include "common/json.h"

int mr_queue_init(mr_queue *q) {
    q->items = NULL;
    q->head = q->count = q->cap = 0;
    if (pipe(q->wake) < 0) return -errno;
    for (int i = 0; i < 2; i++) {
        mc_set_nonblocking(q->wake[i], true);
        mc_set_cloexec(q->wake[i]);
    }
    pthread_mutex_init(&q->lock, NULL);
    return 0;
}

void mr_queue_free(mr_queue *q) {
    struct json_object *event;
    while ((event = mr_queue_pop(q))) json_object_put(event);
    free(q->items);
    q->items = NULL;
    close(q->wake[0]);
    close(q->wake[1]);
    pthread_mutex_destroy(&q->lock);
}

int mr_queue_push(mr_queue *q, struct json_object *event) {
    pthread_mutex_lock(&q->lock);
    int rc = 0;
    if (q->count == q->cap) {
        size_t cap = q->cap ? q->cap * 2 : 32;
        struct json_object **items = cap <= MR_QUEUE_MAX ? malloc(cap * sizeof *items) : NULL;
        if (!items) {
            rc = q->cap >= MR_QUEUE_MAX ? -ENOBUFS : -ENOMEM;
        } else {
            for (size_t i = 0; i < q->count; i++) items[i] = q->items[(q->head + i) % q->cap];
            free(q->items);
            q->items = items;
            q->cap = cap;
            q->head = 0;
        }
    }
    if (rc == 0) {
        q->items[(q->head + q->count) % q->cap] = event;
        q->count++;
    }
    pthread_mutex_unlock(&q->lock);
    if (rc) json_object_put(event);
    else mr_queue_wake(q);
    return rc;
}

struct json_object *mr_queue_pop(mr_queue *q) {
    pthread_mutex_lock(&q->lock);
    struct json_object *event = NULL;
    if (q->count) {
        event = q->items[q->head];
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }
    pthread_mutex_unlock(&q->lock);
    return event;
}

int mr_queue_fd(const mr_queue *q) { return q->wake[0]; }

void mr_queue_drain(mr_queue *q) {
    char bytes[256];
    while (read(q->wake[0], bytes, sizeof bytes) > 0) {
    }
}

void mr_queue_wake(mr_queue *q) {
    char byte = 1;
    /* A full pipe already means the loop will wake. */
    ssize_t ignored = write(q->wake[1], &byte, 1);
    (void)ignored;
}
