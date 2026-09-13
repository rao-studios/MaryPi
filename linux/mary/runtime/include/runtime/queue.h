/* How maryd's threads reach its main loop. The turn, transcription, audio and key
 * workers never touch the daemon's state: each one pushes a JSON event here and the
 * main loop, which owns everything, pops and handles them in order. A byte written to
 * a pipe per push wakes the loop's poll(2). */
#ifndef MARY_RUNTIME_QUEUE_H
#define MARY_RUNTIME_QUEUE_H

#include <pthread.h>
#include <stddef.h>

struct json_object;

#define MR_QUEUE_MAX 65536

typedef struct mr_queue {
    pthread_mutex_t lock;
    struct json_object **items;
    size_t head, count, cap;
    int wake[2];                /* [0] is polled by the main loop */
} mr_queue;

/* 0, or -errno. */
int mr_queue_init(mr_queue *q);
/* Releases whatever was never popped. */
void mr_queue_free(mr_queue *q);
/* Takes the reference. 0; -ENOBUFS past MR_QUEUE_MAX or -ENOMEM (the event is released). */
int mr_queue_push(mr_queue *q, struct json_object *event);
/* The oldest event, now the caller's; NULL when empty. */
struct json_object *mr_queue_pop(mr_queue *q);
/* The fd to poll for POLLIN. */
int mr_queue_fd(const mr_queue *q);
/* Reads the wake bytes; call before popping. */
void mr_queue_drain(mr_queue *q);
/* Wakes the loop without an event; async-signal-safe. */
void mr_queue_wake(mr_queue *q);

#endif
