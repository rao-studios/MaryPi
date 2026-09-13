#include "runtime/transcriber.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "common/frame.h"
#include "common/io.h"
#include "common/json.h"

struct mr_transcriber {
    int fd;
    pthread_t reader;
    mr_transcript_events ev;
    void *user;
    atomic_bool cancelled, closed;
    bool done;
};

static int on_frame(uint8_t kind, const unsigned char *p, size_t len, void *user) {
    struct mr_transcriber *t = user;
    if (kind != MC_FRAME_JSON || atomic_load(&t->cancelled)) return atomic_load(&t->cancelled);
    struct json_object *msg = mc_json_parse((const char *)p, len);
    const char *type = mc_json_type(msg), *text = mc_json_string(msg, "text");
    int stop = 0;
    if (!type) {
    } else if (strcmp(type, "transcript.delta") == 0) {
        if (t->ev.delta) t->ev.delta(text ? text : "", t->user);
    } else if (strcmp(type, "transcript.done") == 0) {
        t->done = true;
        if (t->ev.done) t->ev.done(text ? text : "", t->user);
        stop = 1;
    } else if (strcmp(type, "error") == 0) {
        const char *stage = mc_json_string(msg, "stage"), *message = mc_json_string(msg, "message");
        if (t->ev.error) t->ev.error(stage ? stage : "transcribe", message ? message : "transcription failed", t->user);
    }
    json_object_put(msg);
    return stop;
}

static void *read_events(void *arg) {
    struct mr_transcriber *t = arg;
    mc_frame_reader reader;
    mc_frame_reader_init(&reader, 0, false);
    int rc;
    while ((rc = mc_frame_reader_read_fd(&reader, t->fd, on_frame, t)) == MC_IO_OK) {
    }
    mc_frame_reader_free(&reader);
    if (!t->done && !atomic_load(&t->cancelled) && t->ev.error)
        t->ev.error("transcribe", rc < 0 ? strerror(-rc) : "sewnd ended the transcription early", t->user);
    atomic_store(&t->closed, true);
    if (t->ev.closed) t->ev.closed(t->user);
    return NULL;
}

static int send_type(int fd, const char *type) {
    struct json_object *msg = json_object_new_object();
    json_object_object_add(msg, "type", json_object_new_string(type));
    int rc = mc_frame_write_json(fd, msg);
    json_object_put(msg);
    return rc;
}

mr_transcriber *mr_transcriber_open(const char *sewn_socket, int sample_rate, const mr_transcript_events *events,
                                    void *user, int *error) {
    struct mr_transcriber *t = calloc(1, sizeof *t);
    if (!t) {
        if (error) *error = -ENOMEM;
        return NULL;
    }
    t->ev = *events;
    t->user = user;
    atomic_init(&t->cancelled, false);
    atomic_init(&t->closed, false);
    t->fd = mc_connect_unix(sewn_socket);
    int rc = t->fd < 0 ? t->fd : 0;
    if (rc == 0) {
        /* Audio never waits on a stalled sewnd for long; the frames behind it are dropped. */
        struct timeval patience = { .tv_sec = 2 };
        setsockopt(t->fd, SOL_SOCKET, SO_SNDTIMEO, &patience, sizeof patience);
        struct json_object *start = json_object_new_object();
        json_object_object_add(start, "type", json_object_new_string("transcribe.start"));
        json_object_object_add(start, "sample_rate", json_object_new_int(sample_rate));
        rc = mc_frame_write_json(t->fd, start);
        json_object_put(start);
    }
    if (rc == 0 && pthread_create(&t->reader, NULL, read_events, t) != 0) rc = -EAGAIN;
    if (rc < 0) {
        if (t->fd >= 0) close(t->fd);
        free(t);
        if (error) *error = rc;
        return NULL;
    }
    return t;
}

int mr_transcriber_send(mr_transcriber *t, const int16_t *samples, size_t count) {
    unsigned char bytes[2048];
    while (count) {
        size_t n = count < sizeof bytes / 2 ? count : sizeof bytes / 2;
        for (size_t i = 0; i < n; i++) {
            uint16_t v = (uint16_t)samples[i];
            bytes[2 * i] = (unsigned char)v;
            bytes[2 * i + 1] = (unsigned char)(v >> 8);
        }
        int rc = mc_frame_write_fd(t->fd, MC_FRAME_PCM, bytes, n * 2);
        if (rc < 0) return rc;
        samples += n;
        count -= n;
    }
    return 0;
}

int mr_transcriber_end(mr_transcriber *t) { return send_type(t->fd, "transcribe.end"); }

void mr_transcriber_cancel(mr_transcriber *t) {
    if (!t) return;
    atomic_store(&t->cancelled, true);
    shutdown(t->fd, SHUT_RDWR);
}

bool mr_transcriber_closed(const mr_transcriber *t) { return atomic_load(&((struct mr_transcriber *)t)->closed); }

void mr_transcriber_free(mr_transcriber *t) {
    if (!t) return;
    pthread_join(t->reader, NULL);
    close(t->fd);
    free(t);
}
