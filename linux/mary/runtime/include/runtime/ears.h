/* maryd's ears: MaryVoice's pipeline from the microphone to a transcript
 * (VoicePipeline, WakeWordListener, the VAD and the STT seam), on a thread of its
 * own. Frames arrive from PipeWire's callback — or from a test — through a lock-free
 * ring; the thread runs the keyword spotter, the VAD and the transcription, and
 * posts what it finds to maryd's queue:
 *
 *   ears.wake{keyword}                      "Hey Mary" was spotted; a session opens
 *   ears.state{state, session}              listening | hearing | transcribing
 *   ears.level{rms}                         while listening, about 16 Hz
 *   ears.partial{text, session}             what Voxtral has heard so far
 *   ears.heard{text, woke, follow_up, session}   the utterance; the ears pause until told
 *   ears.timeout{session}                   nothing said; back to standby
 *   ears.error{stage, message, session}     the session failed; back to standby
 *
 * Standby spots the wake word when there is a spotter and wake is on; otherwise it
 * only keeps the pre-roll. maryd decides what comes next: mute for a turn, listen
 * again, or standby. */
#ifndef MARY_RUNTIME_EARS_H
#define MARY_RUNTIME_EARS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime/queue.h"
#include "voice/kws.h"

#define MR_EARS_RATE 16000
#define MR_EARS_FRAME 320               /* 20 ms */
#define MR_EARS_PREROLL_FRAMES 30       /* VADConfig.preRollMs 600 */

typedef struct mr_ears_config {
    const char *sewn_socket;
    const mv_kws_config *kws;       /* NULL: no wake word */
    bool wake;                      /* spot "Hey Mary" in standby */
    int listen_timeout_ms;          /* 6000: silence this long ends a session */
    int max_utterance_ms;           /* 30000 */
} mr_ears_config;

typedef struct mr_ears mr_ears;

/* Starts the thread (and loads the spotter when config->kws is given). NULL with *error set. */
mr_ears *mr_ears_new(const mr_ears_config *config, mr_queue *events, int *error);
void mr_ears_free(mr_ears *ears);

/* 16 kHz mono s16, any count. Real-time safe: PipeWire's callback calls it. */
void mr_ears_hear(mr_ears *ears, const int16_t *samples, size_t count);

/* Open a session now, without the wake word: the Ask Mary button, or the follow-up
 * window after a spoken reply. Ignored while one is open. */
void mr_ears_listen(mr_ears *ears, bool follow_up);
/* End any session; spot the wake word again. */
void mr_ears_standby(mr_ears *ears);
/* End any session and ignore the microphone: Mary is thinking or speaking. */
void mr_ears_mute(mr_ears *ears);
void mr_ears_set_wake(mr_ears *ears, bool on);
/* A spotter loaded and wake on. */
bool mr_ears_can_wake(const mr_ears *ears);

/* What a transcript means (WakePlanner on the utterance the ears heard). */
typedef enum mr_heard_kind {
    MR_HEARD_NOTHING,       /* empty, or the name alone: keep listening */
    MR_HEARD_REQUEST,       /* run a turn with `request` */
    MR_HEARD_STOP,          /* "stop listening": back to standby */
} mr_heard_kind;

/* After a wake the name leads the transcript and is taken off; when the spotter
 * heard the name but Voxtral's transcript lost it, the whole text is the request.
 * Without a wake (the button, a follow-up) the whole text is the request. */
mr_heard_kind mr_heard(const char *text, bool woke, char *request, size_t cap);

#endif
