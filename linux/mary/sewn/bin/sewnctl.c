/* sewnctl: sewnd's key from a terminal. `set` reads the key from standard input,
 * with echo off on a terminal — never from the command line, where any user
 * could read it in /proc. */
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
    fprintf(to, "usage: sewnctl [--socket PATH] status | verify | set\n"
                "  set reads the Mistral API key from standard input, never from the command line\n");
}

int main(int argc, char **argv) {
    const char *socket_path = sewn_default_socket(), *command = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) socket_path = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { usage(stdout); return 0; }
        else if (!command) command = argv[i];
        else { usage(stderr); return 2; }
    }
    if (!command || (strcmp(command, "status") && strcmp(command, "verify") && strcmp(command, "set"))) {
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
