/* threadd: MaryOS's memory, the hard drive's own record of everything on it. It serves
 * thread.v1 over gRPC on thread.sock and the MaryOS ops as JSON lines on local.sock —
 * never the network — with the caller's login name as the owner of everything it reads
 * and writes (the sewn user may name the owner it writes for). Embeddings and graph
 * extraction come from sewnd. systemd runs it as the `thread` user with its state in
 * $STATE_DIRECTORY and its sockets in $RUNTIME_DIRECTORY. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/io.h"
#include "common/log.h"
#include "common/peer.h"
#include "thread/local.h"
#include "thread/service.h"
#include "thread/store.h"

struct connection {
    thread_store *store;
    int fd;
    bool local;
    uid_t trusted_uid;
};

static void *serve(void *arg) {
    struct connection *c = arg;
    uid_t uid;
    thread_caller caller = { .store = c->store };
    if (mc_peer_credentials(c->fd, &uid, NULL, NULL) == 0) {
        mc_user_name(uid, caller.owner, sizeof caller.owner);
        caller.trusted = c->trusted_uid != (uid_t)-1 && uid == c->trusted_uid;
        if (c->local) thread_local_serve(c->fd, &caller);
        else conduit_serve(c->fd, thread_routes, thread_route_count, &caller);
    } else {
        mc_log(MC_LOG_WARNING, "closed a connection whose credentials could not be read");
    }
    close(c->fd);
    free(c);
    return NULL;
}

static volatile sig_atomic_t stopping;
static void on_term(int sig) { stopping = 1; }

int main(int argc, char **argv) {
    static char runtime_socket[256], runtime_local[256];
    const char *runtime = getenv("RUNTIME_DIRECTORY"), *state = getenv("STATE_DIRECTORY");
    const char *socket_path = THREAD_SOCKET_PATH, *local_path = THREAD_LOCAL_SOCKET_PATH;
    const char *state_dir = state && *state ? state : THREAD_STATE_DIR;
    const char *sewn_socket = NULL;
    bool enrich = true;
    if (runtime && *runtime) {
        snprintf(runtime_socket, sizeof runtime_socket, "%s/thread.sock", runtime);
        snprintf(runtime_local, sizeof runtime_local, "%s/local.sock", runtime);
        socket_path = runtime_socket;
        local_path = runtime_local;
    }
    long mode = 0660;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) socket_path = argv[++i];
        else if (strcmp(argv[i], "--local-socket") == 0 && i + 1 < argc) local_path = argv[++i];
        else if (strcmp(argv[i], "--state-dir") == 0 && i + 1 < argc) state_dir = argv[++i];
        else if (strcmp(argv[i], "--sewn-socket") == 0 && i + 1 < argc) sewn_socket = argv[++i];
        else if (strcmp(argv[i], "--socket-mode") == 0 && i + 1 < argc) mode = strtol(argv[++i], NULL, 8);
        else if (strcmp(argv[i], "--no-enrich") == 0) enrich = false;
        else {
            fprintf(strcmp(argv[i], "--help") ? stderr : stdout,
                    "usage: threadd [--socket PATH] [--local-socket PATH] [--state-dir DIR] [--sewn-socket PATH] [--socket-mode OCTAL] [--no-enrich]\n");
            return strcmp(argv[i], "--help") ? 2 : 0;
        }
    }
    mc_log_init("threadd");
    mc_ignore_sigpipe();
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    umask(0077);
    thread_embedder embedder = { 0 };
    if (enrich) thread_sewn_embedder_init(&embedder, sewn_socket);
    thread_store_options options = { .dir = state_dir, .embedder = enrich ? &embedder : NULL, .worker = enrich };
    int error = 0;
    thread_store *store = thread_store_open_with(&options, &error);
    if (!store) {
        mc_log(MC_LOG_ERROR, "cannot open the store in %s: %s", state_dir, strerror(-error));
        return 1;
    }
    int listener = mc_listen_unix(socket_path, (int)mode);
    if (listener < 0) {
        mc_log(MC_LOG_ERROR, "cannot listen on %s: %s", socket_path, strerror(-listener));
        return 1;
    }
    int local = mc_listen_unix(local_path, (int)mode);
    if (local < 0) {
        mc_log(MC_LOG_ERROR, "cannot listen on %s: %s", local_path, strerror(-local));
        return 1;
    }
    struct passwd *sewn = getpwnam(THREAD_SEWN_USER);
    uid_t trusted = sewn ? sewn->pw_uid : (uid_t)-1;
    mc_log(MC_LOG_NOTICE, "node %s listening on %s and %s%s", thread_store_node_id(store), socket_path, local_path,
           enrich ? "" : " (no enrichment)");
    struct pollfd fds[2] = { { listener, POLLIN, 0 }, { local, POLLIN, 0 } };
    while (!stopping) {
        int ready = poll(fds, 2, 1000);
        if (ready < 0) {
            if (errno == EINTR) continue;
            mc_log(MC_LOG_ERROR, "poll: %s", strerror(errno));
            sleep(1);
            continue;
        }
        for (int i = 0; i < 2; i++) {
            if (!(fds[i].revents & POLLIN)) continue;
            int fd = accept(fds[i].fd, NULL, NULL);
            if (fd < 0) {
                if (errno != EINTR && errno != ECONNABORTED) mc_log(MC_LOG_ERROR, "accept: %s", strerror(errno));
                continue;
            }
            mc_set_cloexec(fd);
            struct connection *c = malloc(sizeof *c);
            if (!c) {
                close(fd);
                continue;
            }
            *c = (struct connection){ store, fd, i == 1, trusted };
            pthread_t thread;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&thread, &attr, serve, c) != 0) {
                close(fd);
                free(c);
            }
            pthread_attr_destroy(&attr);
        }
    }
    mc_log(MC_LOG_NOTICE, "stopping: checkpointing thread.db");
    close(listener);
    close(local);
    thread_store_close(store);
    if (enrich) thread_sewn_embedder_free(&embedder);
    return 0;
}
