/* threadd: MaryOS's memory. It serves thread.v1's Index, Library and Documents over
 * gRPC on a unix socket — never the network yet — with the caller's login name as
 * the owner of everything it reads and writes. systemd runs it as the `thread`
 * user with its state in $STATE_DIRECTORY and its socket in $RUNTIME_DIRECTORY. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/io.h"
#include "common/log.h"
#include "common/peer.h"
#include "thread/service.h"
#include "thread/store.h"

struct connection {
    thread_store *store;
    int fd;
};

static void *serve(void *arg) {
    struct connection *c = arg;
    uid_t uid;
    thread_caller caller = { .store = c->store };
    if (mc_peer_credentials(c->fd, &uid, NULL, NULL) == 0) {
        mc_user_name(uid, caller.owner, sizeof caller.owner);
        conduit_serve(c->fd, thread_routes, thread_route_count, &caller);
    } else {
        mc_log(MC_LOG_WARNING, "closed a connection whose credentials could not be read");
    }
    close(c->fd);
    free(c);
    return NULL;
}

int main(int argc, char **argv) {
    static char runtime_socket[256];
    const char *runtime = getenv("RUNTIME_DIRECTORY"), *state = getenv("STATE_DIRECTORY");
    const char *socket_path = THREAD_SOCKET_PATH, *state_dir = state && *state ? state : THREAD_STATE_DIR;
    if (runtime && *runtime) {
        snprintf(runtime_socket, sizeof runtime_socket, "%s/thread.sock", runtime);
        socket_path = runtime_socket;
    }
    long mode = 0660;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) socket_path = argv[++i];
        else if (strcmp(argv[i], "--state-dir") == 0 && i + 1 < argc) state_dir = argv[++i];
        else if (strcmp(argv[i], "--socket-mode") == 0 && i + 1 < argc) mode = strtol(argv[++i], NULL, 8);
        else {
            fprintf(strcmp(argv[i], "--help") ? stderr : stdout, "usage: threadd [--socket PATH] [--state-dir DIR] [--socket-mode OCTAL]\n");
            return strcmp(argv[i], "--help") ? 2 : 0;
        }
    }
    mc_log_init("threadd");
    mc_ignore_sigpipe();
    umask(0077);
    int error = 0;
    thread_store *store = thread_store_open(state_dir, &error);
    if (!store) {
        mc_log(MC_LOG_ERROR, "cannot open the store in %s: %s", state_dir, strerror(-error));
        return 1;
    }
    int listener = mc_listen_unix(socket_path, (int)mode);
    if (listener < 0) {
        mc_log(MC_LOG_ERROR, "cannot listen on %s: %s", socket_path, strerror(-listener));
        return 1;
    }
    mc_log(MC_LOG_NOTICE, "node %s listening on %s", thread_store_node_id(store), socket_path);
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            mc_log(MC_LOG_ERROR, "accept: %s", strerror(errno));
            sleep(1);
            continue;
        }
        mc_set_cloexec(fd);
        struct connection *c = malloc(sizeof *c);
        pthread_t thread;
        pthread_attr_t attr;
        if (!c) {
            close(fd);
            continue;
        }
        *c = (struct connection){ store, fd };
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&thread, &attr, serve, c) != 0) {
            close(fd);
            free(c);
        }
        pthread_attr_destroy(&attr);
    }
}
