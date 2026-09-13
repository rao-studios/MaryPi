/* MaryVoice/VAD/WakePlanner.swift and EndpointHold.swift in C: the pure matchers
 * around the wake word.
 *
 * On MaryOS a keyword spotter hears "Hey Mary" on-device and only then does audio
 * go to Voxtral (PORTING.md deviation 1); these still decide what the transcript
 * means — the request after the name, whether a live partial can still become a
 * wake phrase, and the exact "stop listening" exit.
 *
 * Tokens split on whitespace and normalize to letters and digits, so "Mary," counts
 * and "summary" never does. Letters are ASCII, plus the Latin, Greek, Cyrillic and
 * CJK blocks; dashes, quotes and ellipses are punctuation. */
#ifndef MARY_VOICE_WAKE_H
#define MARY_VOICE_WAKE_H

#include <stdbool.h>
#include <stddef.h>

typedef enum mv_wake {
    MV_WAKE_NONE,       /* not a wake phrase: the name must lead */
    MV_WAKE_BARE,       /* "Hey Mary." — greet and listen */
    MV_WAKE_REQUEST,    /* "Hey Mary, open mail" — the remainder is the first turn */
} mv_wake;

/* WakePlanner.wake(in:). For a request, `request` gets the remainder with its casing
 * and punctuation, trimmed of whitespace and ,;:—–-…. at both ends. */
mv_wake mv_wake_in(const char *text, char *request, size_t cap);
/* couldStillWake(partial:): false once a live partial can no longer wake. */
bool mv_could_still_wake(const char *partial);
/* isStopListening: "stop listening" or "quit listening", once address words are gone. */
bool mv_is_stop_listening(const char *text);

/* EndpointHold.danglingExtension, and extraSilence(forPartial:). */
#define MV_DANGLING_EXTENSION 0.7
double mv_endpoint_extra_silence(const char *partial);

#endif
