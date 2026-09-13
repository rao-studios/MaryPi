/* maryd's composition root (MaryRuntime): one main loop owning every piece of
 * state — the desktop socket, the conversation, the turn, the ears, the speaker
 * and the skill pipes — fed by a queue its worker threads post to.
 *
 * desktop → maryd   ask{text}, listen, stop, dismiss, key.set{key}, key.verify, key.status,
 *                   config{wake}, skills{apps}, skill.result{…}; maryctl adds
 *                   skills.list and skill.call{app, skill, args}
 * maryd → clients   hello{state, key_present, wake, tail}, wake, state{state}, level{rms},
 *                   transcript{text, final, source?}, reply.delta{text}, reply.end{cancelled},
 *                   key.status{…}, error{stage, message}, skill.invoke{…} (desktop only),
 *                   skills{apps} and skill.result{…} (to the maryctl that asked)
 *
 * A turn: the question joins the history, brain builds turn.start, sewnd streams the
 * reply (reply.delta) and its voice (the speaker's ring), the state is speaking from
 * the first audio, and at turn.end the exchange is deposited in Thread. A spoken
 * question is followed, once the speaker is quiet and 300 ms more have passed, by a
 * follow-up session; a typed one goes back to idle. */
#ifndef MARY_RUNTIME_DAEMON_H
#define MARY_RUNTIME_DAEMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "voice/kws.h"

typedef struct mr_config {
    const char *desktop_socket;     /* NULL: $XDG_RUNTIME_DIR/mary/mary.sock */
    const char *sewn_socket;        /* NULL: $SEWN_SOCKET or /run/sewn/sewn.sock */
    const char *thread_socket;      /* NULL: $THREAD_SOCKET or /run/thread/thread.sock */
    bool audio;                     /* open PipeWire */
    bool wake;                      /* listen for "Hey Mary" */
    const mv_kws_config *kws;       /* NULL: no spotter */
    int listen_timeout_ms;          /* 6000: a session nobody speaks into, and the follow-up window */
    int echo_tail_ms;               /* 300: the microphone waits this long after the speaker */
    int skill_timeout_ms;           /* 10000 */
} mr_config;

mr_config mr_config_default(void);

typedef struct mr_daemon mr_daemon;

/* Listens on the desktop socket and starts the ears (and the speaker, with audio).
 * NULL with *error set. */
mr_daemon *mr_daemon_new(const mr_config *config, int *error);
/* Runs until mr_daemon_stop. 0. */
int mr_daemon_run(mr_daemon *d);
/* Async-signal-safe. */
void mr_daemon_stop(mr_daemon *d);
/* Waits (briefly) for worker threads, then frees. */
void mr_daemon_free(mr_daemon *d);

/* Microphone audio, 16 kHz mono s16: what PipeWire's capture delivers, and what a test feeds. */
void mr_daemon_hear(mr_daemon *d, const int16_t *samples, size_t count);

#endif
