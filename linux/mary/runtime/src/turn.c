#include "runtime/turn.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "common/frame.h"
#include "common/io.h"
#include "common/json.h"

struct mr_turn {
    int fd;
    pthread_t thread;
    mr_turn_events ev;
    void *user;
    atomic_bool cancelled;
    bool completed;
    char *start;
    size_t start_len;
    float *pcm;
    size_t pcm_cap;
};

static int on_frame(uint8_t kind, const unsigned char *p, size_t len, void *user) {
    struct mr_turn *t = user;
    if (atomic_load(&t->cancelled)) return 1;
    if (kind == MC_FRAME_PCM) {
        size_t n = len / 4;
        if (n > t->pcm_cap) {
            float *grown = realloc(t->pcm, n * sizeof *grown);
            if (!grown) return 1;
            t->pcm = grown;
            t->pcm_cap = n;
        }
        for (size_t i = 0; i < n; i++) {
            const unsigned char *b = p + 4 * i;
            uint32_t bits = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
            memcpy(&t->pcm[i], &bits, sizeof bits);
        }
        if (n && t->ev.audio) t->ev.audio(t->pcm, n, t->user);
        return 0;
    }
    if (kind != MC_FRAME_JSON) return 0;
    struct json_object *msg = mc_json_parse((const char *)p, len);
    const char *type = mc_json_type(msg);
    int stop = 0;
    if (!type) {
    } else if (strcmp(type, "token") == 0) {
        const char *text = mc_json_string(msg, "text");
        if (text && *text && t->ev.token) t->ev.token(text, t->user);
    } else if (strcmp(type, "tts.failed") == 0) {
        if (t->ev.tts_failed) t->ev.tts_failed(t->user);
    } else if (strcmp(type, "error") == 0) {
        const char *stage = mc_json_string(msg, "stage"), *message = mc_json_string(msg, "message");
        if (t->ev.error) t->ev.error(stage ? stage : "sewnd", message ? message : "sewnd reported an error", t->user);
    } else if (strcmp(type, "turn.end") == 0) {
        t->completed = true;
        stop = 1;
    }
    json_object_put(msg);
    return stop;
}

static void *run(void *arg) {
    struct mr_turn *t = arg;
    int rc = mc_frame_write_fd(t->fd, MC_FRAME_JSON, (const unsigned char *)t->start, t->start_len);
    if (rc == 0) {
        mc_frame_reader reader;
        mc_frame_reader_init(&reader, 0, false);
        while ((rc = mc_frame_reader_read_fd(&reader, t->fd, on_frame, t)) == MC_IO_OK) {
        }
        mc_frame_reader_free(&reader);
    }
    if (rc < 0 && !atomic_load(&t->cancelled) && t->ev.error)
        t->ev.error("sewnd", rc == -EAGAIN ? "sewnd stopped answering" : strerror(-rc), t->user);
    if (t->ev.end) t->ev.end(t->completed && !atomic_load(&t->cancelled), t->user);
    return NULL;
}

mr_turn *mr_turn_start(const char *sewn_socket, struct json_object *turn_start, const mr_turn_events *events,
                       void *user, int *error) {
    struct mr_turn *t = calloc(1, sizeof *t);
    size_t len = 0;
    const char *text = turn_start ? mc_json_compact(turn_start, &len) : NULL;
    if (!t || !text || !(t->start = malloc(len))) {
        free(t);
        if (error) *error = text ? -ENOMEM : -EINVAL;
        return NULL;
    }
    memcpy(t->start, text, len);
    t->start_len = len;
    t->ev = *events;
    t->user = user;
    atomic_init(&t->cancelled, false);
    t->fd = mc_connect_unix(sewn_socket);
    if (t->fd < 0) {
        if (error) *error = t->fd;
        free(t->start);
        free(t);
        return NULL;
    }
    struct timeval idle = { .tv_sec = MR_TURN_IDLE_MS / 1000 };
    setsockopt(t->fd, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof idle);
    if (pthread_create(&t->thread, NULL, run, t) != 0) {
        if (error) *error = -EAGAIN;
        close(t->fd);
        free(t->start);
        free(t);
        return NULL;
    }
    return t;
}

void mr_turn_cancel(mr_turn *t) {
    if (!t) return;
    atomic_store(&t->cancelled, true);
    shutdown(t->fd, SHUT_RDWR);
}

bool mr_turn_cancelled(const mr_turn *t) { return atomic_load(&((struct mr_turn *)t)->cancelled); }

void mr_turn_free(mr_turn *t) {
    if (!t) return;
    pthread_join(t->thread, NULL);
    close(t->fd);
    free(t->start);
    free(t->pcm);
    free(t);
}
