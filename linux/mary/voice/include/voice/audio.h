/* Mary's ears and voice through PipeWire (MaryVoice's MicCapture and the speaker's
 * AVAudioEngine on macOS). One capture stream — 16 kHz mono s16, cut into 20 ms
 * frames and handed to a callback on PipeWire's thread — and one playback stream
 * that plays 24 kHz mono float from a ring maryd fills. maryd runs as the desktop's
 * user, so both reach the session's PipeWire and WirePlumber's default devices.
 * Without PipeWire (a Mac build) every call answers -ENOSYS. */
#ifndef MARY_VOICE_AUDIO_H
#define MARY_VOICE_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct mv_audio_config {
    int capture_rate;               /* 16000 */
    size_t capture_frame;           /* 320 samples: 20 ms */
    int playback_rate;              /* 24000: Voxtral TTS */
    size_t playback_seconds;        /* ring size: 60 s of reply */
    const char *app_name;           /* "Mary" */
} mv_audio_config;

mv_audio_config mv_audio_config_default(void);

/* Called on PipeWire's thread for every 20 ms frame. Keep it short. */
typedef void (*mv_capture_fn)(const int16_t *frame, size_t count, void *user);

typedef struct mv_audio mv_audio;

/* Starts PipeWire's thread and both streams. NULL with *error set (-ENOSYS without PipeWire). */
mv_audio *mv_audio_open(const mv_audio_config *config, mv_capture_fn on_frame, void *user, int *error);
void mv_audio_close(mv_audio *audio);

/* Pauses or resumes the microphone stream. 0, or -errno. */
int mv_audio_capture(mv_audio *audio, bool on);
/* Queues reply audio; returns how many samples fit. */
size_t mv_audio_play(mv_audio *audio, const float *samples, size_t count);
/* Stops the reply now: what is queued is dropped. */
void mv_audio_stop_playback(mv_audio *audio);
/* Samples still waiting to reach the speaker. */
size_t mv_audio_queued(const mv_audio *audio);
/* When the speaker last had reply audio, on mc_now_ms()'s clock; 0 if never. */
int64_t mv_audio_last_played_ms(const mv_audio *audio);

#endif
