/* sewnd: the only process on MaryOS that holds the Mistral API key. It listens on
 * a unix socket — never the network — and serves one operation per connection
 * on a thread of its own. systemd runs it as the `sewn` user with its state in
 * $STATE_DIRECTORY and its socket in $RUNTIME_DIRECTORY. */
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
#include <sys/time.h>
#include <unistd.h>

#ifdef HAVE_CURL
#include <curl/curl.h>
#endif

#include "common/io.h"
#include "common/log.h"
#include "sewn/client.h"
#include "sewn/key.h"
#include "sewn/peer.h"
#include "sewn/server.h"

struct connection {
    sewn_service *svc;
    int fd;
};

static void *serve(void *arg) {
    struct connection *c = arg;
    sewn_peer peer;
    if (sewn_peer_of(c->fd, &peer) == 0) sewn_serve_connection(c->svc, c->fd, &peer);
    else mc_log(MC_LOG_WARNING, "closed a connection whose credentials could not be read");
    close(c->fd);
    free(c);
    return NULL;
}

static void usage(FILE *to) {
    fprintf(to, "usage: sewnd [--socket PATH] [--state-dir DIR] [--admin-group NAME] [--socket-mode OCTAL]\n");
}

int main(int argc, char **argv) {
    static char runtime_socket[256];
    static sewn_service svc;
    const char *runtime = getenv("RUNTIME_DIRECTORY"), *state = getenv("STATE_DIRECTORY");
    const char *socket_path = SEWN_SOCKET_PATH;
    if (runtime && *runtime) {
        snprintf(runtime_socket, sizeof runtime_socket, "%s/sewn.sock", runtime);
        socket_path = runtime_socket;
    }
    const char *state_dir = state && *state ? state : SEWN_STATE_DIR;
    const char *admin_group = SEWN_ADMIN_GROUP;
    long mode = 0660;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) socket_path = argv[++i];
        else if (strcmp(argv[i], "--state-dir") == 0 && i + 1 < argc) state_dir = argv[++i];
        else if (strcmp(argv[i], "--admin-group") == 0 && i + 1 < argc) admin_group = argv[++i];
        else if (strcmp(argv[i], "--socket-mode") == 0 && i + 1 < argc) mode = strtol(argv[++i], NULL, 8);
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { usage(stdout); return 0; }
        else { usage(stderr); return 2; }
    }

    mc_log_init("sewnd");
    mc_ignore_sigpipe();
    umask(0077);
#ifdef HAVE_CURL
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
    if (mkdir(state_dir, 0700) < 0 && errno != EEXIST) {
        mc_log(MC_LOG_ERROR, "cannot create %s: %s", state_dir, strerror(errno));
        return 1;
    }
    sewn_service_init(&svc, state_dir);
    svc.admin_group = admin_group;
    int listener = sewn_listen(socket_path, (int)mode);
    if (listener < 0) {
        mc_log(MC_LOG_ERROR, "cannot listen on %s: %s", socket_path, strerror(-listener));
        return 1;
    }
    mc_log(MC_LOG_NOTICE, "listening on %s; a key is %s", socket_path,
           sewn_key_store_present(&svc.keys) ? "stored" : "not set yet");

    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            mc_log(MC_LOG_ERROR, "accept: %s", strerror(errno));
            sleep(1);
            continue;
        }
        mc_set_cloexec(fd);
        /* A client that never sends its first frame does not keep a thread forever. */
        struct timeval patience = { .tv_sec = 30 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof patience);
        struct connection *c = malloc(sizeof *c);
        pthread_attr_t attr;
        pthread_t thread;
        if (!c) {
            close(fd);
            continue;
        }
        *c = (struct connection){ &svc, fd };
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&thread, &attr, serve, c) != 0) {
            mc_log(MC_LOG_ERROR, "could not start a thread for a connection");
            close(fd);
            free(c);
        }
        pthread_attr_destroy(&attr);
    }
}
