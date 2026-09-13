/* One utterance transcribed through sewnd (the wire in sewn/transcribe.h), which
 * relays it to Voxtral Realtime — MaryVoice's VoiceTranscriber seam on MaryOS
 * (PORTING.md deviation 2). One thread sends (open, send, end: maryd's ears); a
 * thread of the transcriber's own reads and runs the callbacks. */
#ifndef MARY_RUNTIME_TRANSCRIBER_H
#define MARY_RUNTIME_TRANSCRIBER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct mr_transcript_events {
    void (*delta)(const char *text, void *user);        /* what Voxtral has heard so far, as it grows */
    void (*done)(const char *text, void *user);         /* the whole utterance */
    void (*error)(const char *stage, const char *message, void *user);
    void (*closed)(void *user);                         /* last, exactly once */
} mr_transcript_events;

typedef struct mr_transcriber mr_transcriber;

/* Connects, sends transcribe.start{sample_rate} and starts the reader. NULL with
 * *error set when sewnd cannot be reached (no callback runs then). */
mr_transcriber *mr_transcriber_open(const char *sewn_socket, int sample_rate, const mr_transcript_events *events,
                                    void *user, int *error);
/* pcm_s16le mono at the session's rate. 0, or -errno. */
int mr_transcriber_send(mr_transcriber *t, const int16_t *samples, size_t count);
/* transcribe.end: the utterance is over; done follows. */
int mr_transcriber_end(mr_transcriber *t);
/* Abandons it: the socket is shut down, which sewnd takes as cancel. Any thread. */
void mr_transcriber_cancel(mr_transcriber *t);
/* true once `closed` has run. */
bool mr_transcriber_closed(const mr_transcriber *t);
/* Waits for the reader, then frees. */
void mr_transcriber_free(mr_transcriber *t);

#endif
