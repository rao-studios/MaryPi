/* maryd's socket for the desktop and maryctl: $XDG_RUNTIME_DIR/mary/mary.sock, mode
 * 0600 in a 0700 directory, newline-delimited JSON with lines of at most 64 KB. Only
 * the user maryd runs as may connect (peer credentials are checked too). Every client
 * gets every broadcast; the one that publishes `skills` is the desktop, which skill
 * calls are sent to. Writes never block the main loop: each client has an outbox, and
 * a client that lets it pass 1 MiB is dropped. Lines may carry the Mistral key
 * (key.set), so a line's bytes are zeroed once handled. */
#ifndef MARY_RUNTIME_DESKTOP_H
#define MARY_RUNTIME_DESKTOP_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>

#include "common/buf.h"
#include "common/lines.h"

#define MR_CLIENTS_MAX 16
#define MR_LINE_MAX (64u << 10)
#define MR_OUTBOX_MAX (1u << 20)

struct json_object;

typedef struct mr_client {
    int fd;                 /* -1: a free slot */
    unsigned id;            /* never reused while maryd runs */
    mc_line_reader lines;
    mc_buf outbox;
    bool desktop;           /* it published skills */
    bool closing;           /* dropped once the current read is handled */
} mr_client;

typedef struct mr_desktop mr_desktop;
/* `message` is borrowed for the call. */
typedef void (*mr_message_fn)(mr_desktop *d, mr_client *client, struct json_object *message, void *user);
typedef void (*mr_client_fn)(mr_desktop *d, mr_client *client, bool connected, void *user);

struct mr_desktop {
    int listener;
    char path[256];
    mr_client clients[MR_CLIENTS_MAX];
    unsigned next_id;
    mr_message_fn on_message;
    mr_client_fn on_client;
    void *user;
};

/* $XDG_RUNTIME_DIR/mary/mary.sock (or $MARY_SOCKET), created 0700 as needed. -ENOENT without XDG_RUNTIME_DIR. */
int mr_desktop_default_socket(char *out, size_t cap, bool create_dir);

int mr_desktop_listen(mr_desktop *d, const char *path, mr_message_fn on_message, mr_client_fn on_client, void *user);
void mr_desktop_close(mr_desktop *d);

/* The listener and every client, for poll(2). Returns how many were written. */
size_t mr_desktop_pollfds(const mr_desktop *d, struct pollfd *fds, size_t cap);
/* Accepts, reads and flushes as poll reported for those fds. */
void mr_desktop_handle(mr_desktop *d, const struct pollfd *fds, size_t count);

/* Queues one line. 0, or -errno (the client is then dropped after this turn of the loop). */
int mr_desktop_send(mr_desktop *d, mr_client *client, struct json_object *message);
void mr_desktop_broadcast(mr_desktop *d, struct json_object *message);
mr_client *mr_desktop_client(mr_desktop *d, unsigned id);
/* The client that last published skills, or NULL. */
mr_client *mr_desktop_the_desktop(mr_desktop *d);

#endif
