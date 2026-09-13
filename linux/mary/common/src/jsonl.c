#include "common/jsonl.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "common/io.h"
#include "common/json.h"
#include "common/lines.h"

struct first {
    struct json_object *reply;
};

static int take_line(const char *line, size_t len, void *user) {
    struct first *f = user;
    f->reply = mc_json_parse(line, len);
    return 1;
}

static void say(char *message, size_t cap, const char *text) {
    if (message && cap) snprintf(message, cap, "%s", text);
}

int mc_jsonl_call(const char *socket_path, int timeout_ms, struct json_object *request, struct json_object **reply, char *message, size_t cap) {
    *reply = NULL;
    int fd = mc_connect_unix(socket_path);
    if (fd < 0) {
        char why[256];
        snprintf(why, sizeof why, "%s is not reachable: %s", socket_path, strerror(-fd));
        say(message, cap, why);
        return fd;
    }
    if (timeout_ms > 0) {
        struct timeval patience = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof patience);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &patience, sizeof patience);
    }
    size_t n = 0;
    const char *text = mc_json_compact(request, &n);
    int rc = mc_write_all(fd, text, n);
    if (rc == 0) rc = mc_write_all(fd, "\n", 1);
    if (rc) {
        close(fd);
        say(message, cap, "the socket hung up before the request was sent");
        return rc;
    }
    mc_line_reader reader;
    mc_line_reader_init(&reader, MC_JSONL_LINE_MAX);
    struct first f = { NULL };
    char buf[65536];
    int status = 0;
    for (;;) {
        ssize_t got = read(fd, buf, sizeof buf);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) {
            status = -errno;
            break;
        }
        if (got == 0) break;
        if (mc_line_reader_feed(&reader, buf, (size_t)got, take_line, &f) == 1) break;
    }
    mc_line_reader_free(&reader);
    close(fd);
    if (!f.reply) {
        say(message, cap, status == -EAGAIN || status == -EWOULDBLOCK ? "no answer in time" : "the socket closed without answering");
        return status == -EAGAIN || status == -EWOULDBLOCK ? -ETIMEDOUT : status ? status : -ECONNRESET;
    }
    *reply = f.reply;
    const char *type = mc_json_type(f.reply);
    if (type && strcmp(type, "error") == 0) {
        const char *why = mc_json_string(f.reply, "message");
        say(message, cap, why ? why : "error");
        return -EIO;
    }
    return 0;
}
