/* maryd's composition root (MaryRuntime): one main loop owning every piece of
 * state — the desktop socket, the conversation, the turn, the ears, the speaker
 * and the skill pipes — fed by a queue its worker threads post to.
 *
 * desktop → maryd   ask{text}, listen, stop, dismiss, key.set{key}, key.verify, key.status,
 *                   config{wake?, voice?}, voices.list, voice.sample{voice_id, text?}, skills{apps},
 *                   skill.result{…}, world{places, focus, activity?}, selection{…}, selection.clear{applicationID},
 *                   app.state.result{…} (ambient/wire.h); maryctl adds skills.list, skill.call{app, skill, args},
 *                   app.state{app}, ambient.state, trace.list, trace.report, triage{text}, abilities.list;
 *                   the desktop answers a confirmation card with skill.confirm.reply{call_id, yes}
 * maryd → clients   hello{state, key_present, wake, voice, tail}, wake, state{state}, level{rms},
 *                   transcript{text, final, source?}, reply.delta{text}, reply.end{cancelled, contribution, retrieved},
 *                   key.status{…}, error{stage, message}, skill.invoke{…} and world.request and app.state{…} (desktop only),
 *                   skill.confirm{call_id, app, skill, args, summary} (the card), reply.end also carries runs[],
 *                   skills{apps}, skill.result{…}, app.state.result{…}, ambient{state}, trace{records},
 *                   trace.report{text}, triage.result{…} and abilities{records} (to the maryctl that asked),
 *                   voices{ok, voices | message} and voice.sample{voice_id, state, message?} (to the client that asked;
 *                   state is asking, playing, done or failed)
 *
 * A turn: the question joins the history, the desktop is asked for the world (world.request; its answer or
 * 150 ms starts the turn), the ambient engine resolves the route (the intent, the lead place, the memory plan's
 * lanes), triage embeds the words against the skill index (built when the desktop publishes its skills) — a
 * unique winner with a safe argument shape dispatches with no model round; an action turn runs Lane B (the
 * silent skills loop, its calls through the desktop's pipes, a protected one parked on the confirmation card)
 * and its answer is spoken; every other turn is Lane A: brain builds turn.start with the ambient section last
 * in the instructions, sewnd streams the
 * reply (reply.delta) and its voice (the speaker's ring), the state is speaking from
 * the first audio, and at turn.end the exchange is deposited in Thread. A spoken
 * question is followed, once the speaker is quiet and 300 ms more have passed, by a
 * follow-up session; a typed one goes back to idle.
 *
 * A reply whose voice fails keeps its text: sewnd's tts.failed becomes error{stage:"speech", message} with
 * Mistral's reason, and the state never turns to error. Voice the speaker stops taking for speaker_stall_ms
 * is dropped with error{stage:"speaker"}, so Mary never stays speaking into a silent speaker, and a speaker
 * that breaks (PipeWire restarting) is opened again once she is idle. */
#ifndef MARY_RUNTIME_DAEMON_H
#define MARY_RUNTIME_DAEMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "voice/kws.h"

/* The speaker and microphone maryd drives: PipeWire's (voice/audio.h) unless a test hands in its own. */
typedef void (*mr_capture_fn)(const int16_t *frame, size_t count, void *user);
typedef struct mr_audio_ops {
    void *(*open)(mr_capture_fn on_frame, void *user, int *error, void *ops_user);    /* NULL with *error set */
    void (*close)(void *audio);
    int (*capture)(void *audio, bool on);
    size_t (*play)(void *audio, const float *samples, size_t count);              /* how many fit */
    void (*stop)(void *audio);                                                    /* drops what is queued */
    size_t (*queued)(void *audio);
    int64_t (*last_played_ms)(void *audio);                                       /* mc_now_ms()'s clock; 0: never */
    bool (*broken)(void *audio);                                                  /* open it again */
} mr_audio_ops;

typedef struct mr_config {
    const char *desktop_socket;     /* NULL: $XDG_RUNTIME_DIR/mary/mary.sock */
    const char *sewn_socket;        /* NULL: $SEWN_SOCKET or /run/sewn/sewn.sock */
    const char *thread_socket;      /* NULL: $THREAD_SOCKET or /run/thread/thread.sock */
    const char *thread_local_socket;/* NULL: $THREAD_LOCAL_SOCKET or /run/thread/local.sock (ability, behaviour and routing records) */
    bool audio;                     /* open PipeWire */
    bool wake;                      /* listen for "Hey Mary" */
    const mv_kws_config *kws;       /* NULL: no spotter */
    int listen_timeout_ms;          /* 6000: a session nobody speaks into, and the follow-up window */
    int echo_tail_ms;               /* 300: the microphone waits this long after the speaker */
    int skill_timeout_ms;           /* 10000 */
    int speaker_stall_ms;           /* 2000: queued voice the speaker has not taken for this long is dropped */
    const mr_audio_ops *audio_ops;  /* NULL: PipeWire */
    void *audio_user;               /* handed to audio_ops->open */
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
