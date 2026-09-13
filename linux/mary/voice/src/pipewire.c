#include "voice/audio.h"

#include <errno.h>
#include <stdlib.h>

mv_audio_config mv_audio_config_default(void) {
    return (mv_audio_config){ .capture_rate = 16000, .capture_frame = 320, .playback_rate = 24000,
                              .playback_seconds = 60, .app_name = "Mary" };
}

#ifdef HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <stdatomic.h>
#include <string.h>

#include "common/io.h"
#include "common/log.h"
#include "voice/ring.h"

struct mv_audio {
    struct pw_thread_loop *loop;
    struct pw_stream *capture;
    struct pw_stream *playback;
    mv_framer framer;
    mv_ring ring;
    mv_capture_fn on_frame;
    void *user;
    atomic_bool reset_framer;       /* set when capture pauses; the capture callback clears the partial frame */
    atomic_llong last_played_ms;
};

static void on_capture(void *data) {
    struct mv_audio *a = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(a->capture);
    if (!b) return;
    struct spa_data *d = &b->buffer->datas[0];
    if (atomic_exchange(&a->reset_framer, false)) mv_framer_reset(&a->framer);
    if (d->data && d->chunk) {
        uint32_t offset = d->chunk->offset < d->maxsize ? d->chunk->offset : d->maxsize;
        uint32_t size = d->chunk->size <= d->maxsize - offset ? d->chunk->size : d->maxsize - offset;
        mv_framer_push(&a->framer, (const int16_t *)((uint8_t *)d->data + offset), size / sizeof(int16_t), a->on_frame, a->user);
    }
    pw_stream_queue_buffer(a->capture, b);
}

static void on_playback(void *data) {
    struct mv_audio *a = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(a->playback);
    if (!b) return;
    struct spa_data *d = &b->buffer->datas[0];
    if (!d->data) {
        pw_stream_queue_buffer(a->playback, b);
        return;
    }
    uint32_t frames = d->maxsize / sizeof(float);
    if (b->requested && b->requested < frames) frames = (uint32_t)b->requested;
    float *out = d->data;
    size_t got = mv_ring_read(&a->ring, out, frames);
    if (got) atomic_store(&a->last_played_ms, mc_now_ms());
    memset(out + got, 0, (frames - got) * sizeof(float));   /* silence past the end of the reply */
    d->chunk->offset = 0;
    d->chunk->stride = sizeof(float);
    d->chunk->size = frames * sizeof(float);
    pw_stream_queue_buffer(a->playback, b);
}

static const struct pw_stream_events capture_events = { PW_VERSION_STREAM_EVENTS, .process = on_capture };
static const struct pw_stream_events playback_events = { PW_VERSION_STREAM_EVENTS, .process = on_playback };

static struct pw_stream *open_stream(struct mv_audio *a, bool input, int rate, enum spa_audio_format format,
                                     const char *name, const char *app, const struct pw_stream_events *events) {
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, input ? "Capture" : "Playback",
        PW_KEY_MEDIA_ROLE, "Communication",
        PW_KEY_APP_NAME, app,
        PW_KEY_NODE_NAME, name,
        NULL);
    struct pw_stream *stream = pw_stream_new_simple(pw_thread_loop_get_loop(a->loop), name, props, events, a);
    if (!stream) return NULL;
    uint8_t buffer[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.format = format, .channels = 1, .rate = (uint32_t)rate));
    enum pw_stream_flags flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
    if (pw_stream_connect(stream, input ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0) {
        pw_stream_destroy(stream);
        return NULL;
    }
    return stream;
}

mv_audio *mv_audio_open(const mv_audio_config *config, mv_capture_fn on_frame, void *user, int *error) {
    static bool initialized = false;
    if (!initialized) {
        pw_init(NULL, NULL);
        initialized = true;
    }
    mv_audio_config c = config ? *config : mv_audio_config_default();
    struct mv_audio *a = calloc(1, sizeof *a);
    int rc = a ? 0 : -ENOMEM;
    if (rc == 0) rc = mv_framer_init(&a->framer, c.capture_frame);
    if (rc == 0) rc = mv_ring_init(&a->ring, (size_t)c.playback_rate * c.playback_seconds);
    if (rc == 0 && !(a->loop = pw_thread_loop_new("mary-audio", NULL))) rc = -ENOMEM;
    if (rc) {
        if (a) {
            mv_framer_free(&a->framer);
            mv_ring_free(&a->ring);
            free(a);
        }
        if (error) *error = rc;
        return NULL;
    }
    a->on_frame = on_frame;
    a->user = user;
    pw_thread_loop_lock(a->loop);
    a->capture = open_stream(a, true, c.capture_rate, SPA_AUDIO_FORMAT_S16_LE, "mary-listening", c.app_name, &capture_events);
    a->playback = open_stream(a, false, c.playback_rate, SPA_AUDIO_FORMAT_F32_LE, "mary-speaking", c.app_name, &playback_events);
    pw_thread_loop_unlock(a->loop);
    if (!a->capture || !a->playback || pw_thread_loop_start(a->loop) < 0) {
        mc_log(MC_LOG_ERROR, "PipeWire streams could not be opened");
        mv_audio_close(a);
        if (error) *error = -EIO;
        return NULL;
    }
    return a;
}

void mv_audio_close(mv_audio *a) {
    if (!a) return;
    if (a->loop) pw_thread_loop_stop(a->loop);
    if (a->capture) pw_stream_destroy(a->capture);
    if (a->playback) pw_stream_destroy(a->playback);
    if (a->loop) pw_thread_loop_destroy(a->loop);
    mv_framer_free(&a->framer);
    mv_ring_free(&a->ring);
    free(a);
}

int mv_audio_capture(mv_audio *a, bool on) {
    pw_thread_loop_lock(a->loop);
    int rc = pw_stream_set_active(a->capture, on);
    if (!on) atomic_store(&a->reset_framer, true);   /* PipeWire's thread owns the framer */
    pw_thread_loop_unlock(a->loop);
    return rc < 0 ? rc : 0;
}

size_t mv_audio_play(mv_audio *a, const float *samples, size_t count) { return mv_ring_write(&a->ring, samples, count); }
void mv_audio_stop_playback(mv_audio *a) { mv_ring_request_flush(&a->ring); }
size_t mv_audio_queued(const mv_audio *a) { return mv_ring_available(&a->ring); }
int64_t mv_audio_last_played_ms(const mv_audio *a) { return atomic_load(&((struct mv_audio *)a)->last_played_ms); }

#else

struct mv_audio {
    int unused;
};

mv_audio *mv_audio_open(const mv_audio_config *config, mv_capture_fn on_frame, void *user, int *error) {
    if (error) *error = -ENOSYS;
    return NULL;
}
void mv_audio_close(mv_audio *audio) {}
int mv_audio_capture(mv_audio *audio, bool on) { return -ENOSYS; }
size_t mv_audio_play(mv_audio *audio, const float *samples, size_t count) { return 0; }
void mv_audio_stop_playback(mv_audio *audio) {}
size_t mv_audio_queued(const mv_audio *audio) { return 0; }
int64_t mv_audio_last_played_ms(const mv_audio *audio) { return 0; }
#endif
