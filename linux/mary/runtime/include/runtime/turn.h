/* One turn through sewnd (the wire in sewn/turn.h), run on a thread of its own:
 * MaryRuntime's TextTurnRunner and the voice service's Sewn lane. maryd hands it
 * brain's turn.start; the callbacks run on the turn's thread, so maryd's own
 * callbacks only post events to its queue — except audio, which goes straight to
 * the speaker's ring. Cancelling shuts the socket down, which sewnd takes as
 * cancel (Sewn's barge-in). */
#ifndef MARY_RUNTIME_TURN_H
#define MARY_RUNTIME_TURN_H

#include <stdbool.h>
#include <stddef.h>

struct json_object;

/* A silent sewnd is given up on after this long. */
#define MR_TURN_IDLE_MS 60000

typedef struct mr_turn_events {
    void (*token)(const char *text, void *user);
    void (*audio)(const float *samples, size_t count, void *user);    /* 24 kHz mono, in order */
    void (*tts_failed)(void *user);
    void (*error)(const char *stage, const char *message, void *user);
    void (*end)(bool completed, void *user);    /* last, exactly once: completed means turn.end arrived */
} mr_turn_events;

typedef struct mr_turn mr_turn;

/* Connects and starts the thread; `turn_start` is serialized before this returns.
 * NULL with *error set when sewnd cannot be reached (no callback runs then). */
mr_turn *mr_turn_start(const char *sewn_socket, struct json_object *turn_start, const mr_turn_events *events,
                       void *user, int *error);
/* Stops the turn now; `end` still runs, with completed false. Any thread. */
void mr_turn_cancel(mr_turn *turn);
bool mr_turn_cancelled(const mr_turn *turn);
/* Waits for the thread, then frees. */
void mr_turn_free(mr_turn *turn);

#endif
