#include "runtime/desktop.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/io.h"
#include "common/json.h"
#include "common/log.h"
#include "common/peer.h"
#include "common/secure.h"

int mr_desktop_default_socket(char *out, size_t cap, bool create_dir) {
    const char *explicit_path = getenv("MARY_SOCKET"), *runtime = getenv("XDG_RUNTIME_DIR");
    if (explicit_path && *explicit_path) {
        snprintf(out, cap, "%s", explicit_path);
        return 0;
    }
    if (!runtime || !*runtime) return -ENOENT;
    snprintf(out, cap, "%s/mary", runtime);
    if (create_dir && mkdir(out, 0700) < 0 && errno != EEXIST) return -errno;
    snprintf(out, cap, "%s/mary/mary.sock", runtime);
    return 0;
}

int mr_desktop_listen(mr_desktop *d, const char *path, mr_message_fn on_message, mr_client_fn on_client, void *user) {
    memset(d, 0, sizeof *d);
    for (int i = 0; i < MR_CLIENTS_MAX; i++) d->clients[i].fd = -1;
    snprintf(d->path, sizeof d->path, "%s", path);
    d->on_message = on_message;
    d->on_client = on_client;
    d->user = user;
    d->next_id = 1;
    d->listener = mc_listen_unix(path, 0600);
    if (d->listener < 0) return d->listener;
    mc_set_nonblocking(d->listener, true);
    return 0;
}

static void drop(mr_desktop *d, mr_client *c) {
    if (c->fd < 0) return;
    close(c->fd);
    c->fd = -1;
    mc_line_reader_free(&c->lines);
    mc_buf_free(&c->outbox);
    if (d->on_client) d->on_client(d, c, false, d->user);
    c->desktop = false;
    c->closing = false;
}

void mr_desktop_close(mr_desktop *d) {
    for (int i = 0; i < MR_CLIENTS_MAX; i++) drop(d, &d->clients[i]);
    if (d->listener >= 0) {
        close(d->listener);
        unlink(d->path);
        d->listener = -1;
    }
}

size_t mr_desktop_pollfds(const mr_desktop *d, struct pollfd *fds, size_t cap) {
    size_t n = 0;
    if (d->listener >= 0 && n < cap) fds[n++] = (struct pollfd){ .fd = d->listener, .events = POLLIN };
    for (int i = 0; i < MR_CLIENTS_MAX && n < cap; i++) {
        const mr_client *c = &d->clients[i];
        if (c->fd >= 0) fds[n++] = (struct pollfd){ .fd = c->fd, .events = POLLIN | (c->outbox.len ? POLLOUT : 0) };
    }
    return n;
}

static void accept_clients(mr_desktop *d) {
    for (;;) {
        int fd = accept(d->listener, NULL, NULL);
        if (fd < 0) return;
        mc_set_cloexec(fd);
        uid_t uid;
        gid_t gid;
        pid_t pid;
        mr_client *slot = NULL;
        for (int i = 0; i < MR_CLIENTS_MAX && !slot; i++)
            if (d->clients[i].fd < 0) slot = &d->clients[i];
        if (mc_peer_credentials(fd, &uid, &gid, &pid) < 0 || uid != getuid()) {
            mc_log(MC_LOG_WARNING, "refused a connection from another user");
            close(fd);
            continue;
        }
        if (!slot) {
            mc_log(MC_LOG_WARNING, "refused a connection: %d clients already", MR_CLIENTS_MAX);
            close(fd);
            continue;
        }
        mc_set_nonblocking(fd, true);
        memset(slot, 0, sizeof *slot);
        slot->fd = fd;
        slot->id = d->next_id++;
        mc_line_reader_init(&slot->lines, MR_LINE_MAX);
        if (d->on_client) d->on_client(d, slot, true, d->user);
    }
}

struct line_ctx {
    mr_desktop *d;
    mr_client *c;
};

static int on_line(const char *line, size_t len, void *user) {
    struct line_ctx *ctx = user;
    if (len) {
        struct json_object *msg = mc_json_parse_secret(line, len);
        if (!msg || !mc_json_type(msg)) {
            struct json_object *err = json_object_new_object();
            json_object_object_add(err, "type", json_object_new_string("error"));
            json_object_object_add(err, "stage", json_object_new_string("request"));
            json_object_object_add(err, "message", json_object_new_string("a line that is not a typed JSON message"));
            mr_desktop_send(ctx->d, ctx->c, err);
            json_object_put(err);
        } else if (ctx->d->on_message) {
            ctx->d->on_message(ctx->d, ctx->c, msg, ctx->d->user);
        }
        if (msg) json_object_put(msg);
    }
    mc_secure_zero((char *)line, len);
    return ctx->c->closing;
}

static void flush(mr_client *c) {
    while (c->outbox.len) {
        ssize_t n = write(c->fd, c->outbox.data, c->outbox.len);
        if (n < 0) {
            if (errno != EAGAIN && errno != EINTR) c->closing = true;
            return;
        }
        mc_buf_consume(&c->outbox, (size_t)n);
    }
}

void mr_desktop_handle(mr_desktop *d, const struct pollfd *fds, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (!fds[i].revents) continue;
        if (fds[i].fd == d->listener) {
            accept_clients(d);
            continue;
        }
        mr_client *c = NULL;
        for (int k = 0; k < MR_CLIENTS_MAX && !c; k++)
            if (d->clients[k].fd == fds[i].fd) c = &d->clients[k];
        if (!c) continue;
        if (fds[i].revents & POLLOUT) flush(c);
        if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
            char bytes[16384];
            ssize_t n = read(c->fd, bytes, sizeof bytes);
            if (n > 0) {
                struct line_ctx ctx = { d, c };
                if (mc_line_reader_feed(&c->lines, bytes, (size_t)n, on_line, &ctx) == -EMSGSIZE)
                    mc_log(MC_LOG_WARNING, "dropped a line longer than %u bytes", MR_LINE_MAX);
                mc_secure_zero(bytes, (size_t)n);
                if (c->lines.buf.data && c->lines.buf.cap > c->lines.buf.len)
                    mc_secure_zero(c->lines.buf.data + c->lines.buf.len, c->lines.buf.cap - c->lines.buf.len);
            } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                c->closing = true;
            }
        }
        if (c->closing) drop(d, c);
    }
}

int mr_desktop_send(mr_desktop *d, mr_client *c, struct json_object *message) {
    if (!c || c->fd < 0) return -ENOTCONN;
    size_t len = 0;
    const char *text = mc_json_compact(message, &len);
    if (!text) return -EINVAL;
    if (c->outbox.len + len + 1 > MR_OUTBOX_MAX) {
        mc_log(MC_LOG_WARNING, "client %u is not reading; dropping it", c->id);
        c->closing = true;
        return -ENOBUFS;
    }
    if (mc_buf_append(&c->outbox, text, len) < 0 || mc_buf_append(&c->outbox, "\n", 1) < 0) {
        c->closing = true;
        return -ENOMEM;
    }
    flush(c);
    return c->closing ? -EPIPE : 0;
}

void mr_desktop_broadcast(mr_desktop *d, struct json_object *message) {
    for (int i = 0; i < MR_CLIENTS_MAX; i++)
        if (d->clients[i].fd >= 0 && !d->clients[i].closing) mr_desktop_send(d, &d->clients[i], message);
}

mr_client *mr_desktop_client(mr_desktop *d, unsigned id) {
    for (int i = 0; i < MR_CLIENTS_MAX; i++)
        if (d->clients[i].fd >= 0 && d->clients[i].id == id) return &d->clients[i];
    return NULL;
}

mr_client *mr_desktop_the_desktop(mr_desktop *d) {
    for (int i = 0; i < MR_CLIENTS_MAX; i++)
        if (d->clients[i].fd >= 0 && d->clients[i].desktop) return &d->clients[i];
    return NULL;
}
