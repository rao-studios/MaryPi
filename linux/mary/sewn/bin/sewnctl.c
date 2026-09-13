/* sewnctl: sewnd's key from a terminal. `set` reads the key from standard input,
 * with echo off on a terminal — never from the command line, where any user
 * could read it in /proc. `voices` lists Mistral's voices, and `speak` writes one
 * text's audio to standard output, to hear sewnd and Mistral without maryd. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "common/frame.h"
#include "common/io.h"
#include "common/json.h"
#include "common/secure.h"
#include "sewn/client.h"
#include "sewn/key.h"

static const char *or_else(const char *s, const char *fallback) { return s ? s : fallback; }

static int print_reply(struct json_object *reply) {
    const char *type = mc_json_type(reply);
    if (!type) {
        fprintf(stderr, "sewnctl: sewnd answered with something unexpected\n");
        return 1;
    }
    if (strcmp(type, "error") == 0) {
        fprintf(stderr, "sewnctl: %s: %s\n", or_else(mc_json_string(reply, "stage"), "error"),
                or_else(mc_json_string(reply, "message"), "no message"));
        return 1;
    }
    bool present = false, ok = false;
    int64_t at = 0;
    mc_json_bool(reply, "present", &present);
    printf("key:      %s\n", present ? "stored" : "not set");
    if (mc_json_int64(reply, "verified_at", &at) && at > 0) {
        time_t t = (time_t)(at / 1000);
        struct tm tm;
        char when[40];
        gmtime_r(&t, &tm);
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S UTC", &tm);
        printf("verified: %s\n", when);
    } else {
        printf("verified: never\n");
    }
    if (mc_json_bool(reply, "ok", &ok)) {
        printf("check:    %s\n", or_else(mc_json_string(reply, "message"), ok ? "accepted" : "refused"));
        return ok ? 0 : 1;
    }
    return 0;
}

/* One line from standard input, a byte at a time so no stdio buffer keeps a copy. */
static ssize_t read_secret_line(char *out, size_t cap) {
    size_t n = 0;
    for (;;) {
        char c;
        ssize_t r = read(0, &c, 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (r == 0 || c == '\n') break;
        if (n + 1 >= cap) {
            mc_secure_zero(out, cap);
            return -EMSGSIZE;
        }
        out[n++] = c;
    }
    while (n && out[n - 1] == '\r') n--;
    out[n] = 0;
    return (ssize_t)n;
}

static void usage(FILE *to) {
    fprintf(to, "usage: sewnctl [--socket PATH] status | verify | set | voices | speak VOICE TEXT\n"
                "  set reads the Mistral API key from standard input, never from the command line\n"
                "  speak writes 24 kHz mono float32 to standard output:\n"
                "    sewnctl speak fr_marie_neutral \"Bonjour\" | pw-cat --playback --format f32 --rate 24000 --channels 1 -\n");
}

static int list_voices(const char *socket_path) {
    int fd = sewn_connect(socket_path);
    if (fd < 0) {
        fprintf(stderr, "sewnctl: cannot reach sewnd at %s: %s\n", socket_path, strerror(-fd));
        return 1;
    }
    struct json_object *request = json_object_new_object(), *reply = NULL;
    json_object_object_add(request, "type", json_object_new_string("voices.list"));
    int rc = sewn_call(fd, request, &reply);
    json_object_put(request);
    close(fd);
    if (rc < 0) {
        fprintf(stderr, "sewnctl: %s\n", strerror(-rc));
        return 1;
    }
    const char *type = mc_json_type(reply);
    struct json_object *voices = type && strcmp(type, "voices") == 0 ? mc_json_array(reply, "voices") : NULL;
    if (!voices) {
        int status = print_reply(reply);
        json_object_put(reply);
        return status ? status : 1;
    }
    size_t n = json_object_array_length(voices);
    for (size_t i = 0; i < n; i++) {
        struct json_object *voice = json_object_array_get_idx(voices, i), *languages = mc_json_array(voice, "languages");
        char langs[64] = "";
        for (size_t k = 0; languages && k < json_object_array_length(languages); k++) {
            const char *language = json_object_get_string(json_object_array_get_idx(languages, k));
            size_t used = strlen(langs);
            snprintf(langs + used, sizeof langs - used, "%s%s", k ? "," : "", language ? language : "");
        }
        bool custom = false;
        mc_json_bool(voice, "custom", &custom);
        printf("%-40s %-24s %s%s\n", or_else(mc_json_string(voice, "voice_id"), "?"), or_else(mc_json_string(voice, "name"), ""),
               langs, custom ? "  (yours)" : "");
    }
    if (!n) printf("no voices\n");
    json_object_put(reply);
    return 0;
}

struct spoken {
    size_t bytes;
    int status;
};

static int on_speech(uint8_t kind, const unsigned char *bytes, size_t len, void *user) {
    struct spoken *s = user;
    if (kind == MC_FRAME_PCM) {
        if (fwrite(bytes, 1, len, stdout) != len) {
            s->status = 1;
            return 1;
        }
        s->bytes += len;
        return 0;
    }
    struct json_object *msg = mc_json_parse((const char *)bytes, len);
    const char *type = mc_json_type(msg);
    int stop = 0;
    if (type && strcmp(type, "tts.failed") == 0) {
        fprintf(stderr, "sewnctl: not spoken: %s\n", or_else(mc_json_string(msg, "message"), "Mistral could not speak it"));
        s->status = 1;
    } else if (type && strcmp(type, "error") == 0) {
        fprintf(stderr, "sewnctl: %s: %s\n", or_else(mc_json_string(msg, "stage"), "error"), or_else(mc_json_string(msg, "message"), "no message"));
        s->status = 1;
        stop = 1;
    } else if (type && strcmp(type, "speak.end") == 0) {
        stop = 1;
    }
    json_object_put(msg);
    return stop;
}

/* One text's audio, 24 kHz mono float32, to standard output: pipe it to pw-cat. */
static int speak(const char *socket_path, const char *voice_id, const char *text) {
    if (isatty(1)) {
        fprintf(stderr, "sewnctl: speak writes raw audio; pipe it: | pw-cat --playback --format f32 --rate 24000 --channels 1 -\n");
        return 2;
    }
    int fd = sewn_connect(socket_path);
    if (fd < 0) {
        fprintf(stderr, "sewnctl: cannot reach sewnd at %s: %s\n", socket_path, strerror(-fd));
        return 1;
    }
    struct json_object *request = json_object_new_object();
    json_object_object_add(request, "type", json_object_new_string("speak"));
    json_object_object_add(request, "voice_id", json_object_new_string(voice_id));
    json_object_object_add(request, "text", json_object_new_string(text));
    int rc = mc_frame_write_json(fd, request);
    json_object_put(request);
    struct spoken s = { 0, 0 };
    if (rc == 0) {
        mc_frame_reader reader;
        mc_frame_reader_init(&reader, 0, false);
        int status;
        while ((status = mc_frame_reader_read_fd(&reader, fd, on_speech, &s)) == MC_IO_OK) {}
        mc_frame_reader_free(&reader);
        if (status != MC_IO_STOPPED && !s.status && !s.bytes) rc = status < 0 ? status : -ECONNRESET;
    }
    close(fd);
    fflush(stdout);
    if (rc < 0) {
        fprintf(stderr, "sewnctl: %s\n", strerror(-rc));
        return 1;
    }
    fprintf(stderr, "sewnctl: %zu bytes of 24 kHz float32 audio\n", s.bytes);
    return s.status;
}

int main(int argc, char **argv) {
    const char *socket_path = sewn_default_socket(), *command = NULL, *operands[2] = { NULL, NULL };
    int count = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) socket_path = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { usage(stdout); return 0; }
        else if (!command) command = argv[i];
        else if (count < 2) operands[count++] = argv[i];
        else { usage(stderr); return 2; }
    }
    if (command && strcmp(command, "voices") == 0 && count == 0) return list_voices(socket_path);
    if (command && strcmp(command, "speak") == 0 && count == 2) return speak(socket_path, operands[0], operands[1]);
    if (!command || count || (strcmp(command, "status") && strcmp(command, "verify") && strcmp(command, "set"))) {
        usage(stderr);
        return 2;
    }

    char key[SEWN_KEY_MAX + 2];
    ssize_t key_len = 0;
    if (strcmp(command, "set") == 0) {
        struct termios saved;
        bool tty = isatty(0) && tcgetattr(0, &saved) == 0;
        if (tty) {
            struct termios quiet = saved;
            quiet.c_lflag &= ~(tcflag_t)ECHO;
            tcsetattr(0, TCSANOW, &quiet);
            fprintf(stderr, "Mistral API key: ");
        }
        key_len = read_secret_line(key, sizeof key);
        if (tty) {
            tcsetattr(0, TCSANOW, &saved);
            fputc('\n', stderr);
        }
        if (key_len < 0) {
            fprintf(stderr, "sewnctl: could not read the key: %s\n", strerror((int)-key_len));
            return 1;
        }
        if (!sewn_key_valid(key, (size_t)key_len)) {
            mc_secure_zero(key, sizeof key);
            fprintf(stderr, "sewnctl: that does not look like a Mistral API key\n");
            return 1;
        }
    }

    int fd = sewn_connect(socket_path);
    if (fd < 0) {
        mc_secure_zero(key, sizeof key);
        fprintf(stderr, "sewnctl: cannot reach sewnd at %s: %s\n", socket_path, strerror(-fd));
        return 1;
    }
    struct json_object *reply = NULL;
    int rc;
    if (strcmp(command, "set") == 0) {
        rc = sewn_call_key_set(fd, key, (size_t)key_len, &reply);
        mc_secure_zero(key, sizeof key);
    } else {
        struct json_object *request = json_object_new_object();
        json_object_object_add(request, "type", json_object_new_string(strcmp(command, "status") == 0 ? "key.status" : "key.verify"));
        rc = sewn_call(fd, request, &reply);
        json_object_put(request);
    }
    close(fd);
    if (rc < 0) {
        fprintf(stderr, "sewnctl: %s\n", strerror(-rc));
        return 1;
    }
    int status = print_reply(reply);
    json_object_put(reply);
    return status;
}
