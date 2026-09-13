/* maryd: Mary on MaryOS. A systemd user service, started by the desktop's launcher so
 * the microphone only ever opens inside a graphical session; it serves the desktop on
 * $XDG_RUNTIME_DIR/mary/mary.sock and reaches sewnd and threadd on their sockets. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "common/io.h"
#include "common/log.h"
#include "runtime/daemon.h"

static mr_daemon *running;

static void on_signal(int sig) {
    if (running) mr_daemon_stop(running);
}

static void usage(FILE *to) {
    fprintf(to, "usage: maryd [--socket PATH] [--sewn-socket PATH] [--thread-socket PATH]\n"
                "             [--no-audio] [--no-wake] [--model-dir DIR] [--keywords FILE]\n");
}

int main(int argc, char **argv) {
    mr_config config = mr_config_default();
    mv_kws_config kws = mv_kws_config_default();
    config.kws = &kws;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) config.desktop_socket = argv[++i];
        else if (strcmp(argv[i], "--sewn-socket") == 0 && i + 1 < argc) config.sewn_socket = argv[++i];
        else if (strcmp(argv[i], "--thread-socket") == 0 && i + 1 < argc) config.thread_socket = argv[++i];
        else if (strcmp(argv[i], "--model-dir") == 0 && i + 1 < argc) kws.model_dir = argv[++i];
        else if (strcmp(argv[i], "--keywords") == 0 && i + 1 < argc) kws.keywords_file = argv[++i];
        else if (strcmp(argv[i], "--no-audio") == 0) config.audio = false;
        else if (strcmp(argv[i], "--no-wake") == 0) config.wake = false;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { usage(stdout); return 0; }
        else { usage(stderr); return 2; }
    }

    mc_log_init("maryd");
    mc_ignore_sigpipe();
    umask(0077);
    int error = 0;
    running = mr_daemon_new(&config, &error);
    if (!running) {
        mc_log(MC_LOG_ERROR, "cannot start: %s", error == -ENOENT ? "no XDG_RUNTIME_DIR (run maryd inside a session)" : strerror(-error));
        return 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    mc_log(MC_LOG_NOTICE, "ready");
    mr_daemon_run(running);
    mr_daemon_free(running);
    running = NULL;
    mc_log(MC_LOG_NOTICE, "stopped");
    return 0;
}
