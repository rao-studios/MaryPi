/* sewnd's transcription: one utterance per connection, relayed to Voxtral
 * Realtime over a WebSocket only sewnd can open, because only sewnd holds the key.
 *
 *   client  transcribe.start{sample_rate?, delay_ms?}
 *   client  PCM frames                  pcm_s16le mono at sample_rate, in order
 *   client  transcribe.flush            optional: finalize what has been sent so far
 *   client  transcribe.end              the utterance is over
 *   client  cancel                      abandon it (closing the connection does too)
 *
 *   sewnd   transcribe.ready            Voxtral has the session (audio sent before it is kept)
 *   sewnd   transcript.delta{text}      as Voxtral hears it
 *   sewnd   transcript.done{text}       the whole utterance
 *   sewnd   error{stage, message}       "request", "key", "network", "transcribe"
 *
 * Sample rates are Voxtral's: 8000, 16000, 22050, 44100 or 48000 (16000 otherwise). */
#ifndef MARY_SEWN_TRANSCRIBE_H
#define MARY_SEWN_TRANSCRIBE_H

#include "common/frame.h"
#include "sewn/server.h"

struct json_object;

/* After transcribe.end, how long Voxtral has to send transcription.done. */
#define SEWN_TRANSCRIBE_DONE_WAIT_MS 5000
/* No utterance is longer than this; the session is ended for the client. */
#define SEWN_TRANSCRIBE_MAX_MS (5 * 60 * 1000)

int sewn_run_transcribe(sewn_service *svc, int fd, mc_frame_reader *reader, struct json_object *start);

#endif
