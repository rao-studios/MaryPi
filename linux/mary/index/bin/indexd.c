/* indexd: every file in the home has a record in the Thread. A user unit the desktop
 * launcher starts beside maryd: it reconciles the home with threadd at start (files
 * that changed while it was down), watches the home for saves, renames and
 * deletions, and reconciles again every few hours. It never reads the network and
 * never holds a key; it talks to threadd over /run/thread/local.sock as the user it
 * runs as. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <errno.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/io.h"
#include "common/json.h"
#include "common/log.h"
#include "common/peer.h"
#include "index/client.h"
#include "index/reconcile.h"
#include "index/record.h"
#include "index/walk.h"
#include "index/watch.h"

#define RECONCILE_EVERY_MS (6 * 60 * 60 * 1000)

static volatile sig_atomic_t stopping;
static void on_term(int sig) { stopping = 1; }

static void usage(FILE *to) {
    fprintf(to, "usage: indexd [--root DIR] [--socket PATH] [--once] [--no-watch]\n"
                "  --once      reconcile the home with the Thread and exit\n"
                "  --no-watch  reconcile at start and every six hours, without inotify\n");
}

static int reconcile(const ix_client *client, const char *root, const char *owner) {
    ix_reconcile_stats stats;
    char message[256] = "";
    int rc = ix_reconcile(client, root, owner, &stats, message, sizeof message);
    if (rc) {
        mc_log(MC_LOG_WARNING, "reconcile failed: %s", message);
        return rc;
    }
    mc_log(MC_LOG_NOTICE, "reconcile: %ld seen, %ld missing, %ld stale, %ld orphaned, %ld written, %ld failed, %lld ms (run %lld)",
           stats.seen, stats.missing, stats.stale, stats.orphaned, stats.deposited, stats.failed, (long long)stats.ms, (long long)stats.run_id);
    return 0;
}

static void record(const ix_client *client, const char *root, const char *owner, const char *path) {
    if (!ix_walk_admits(root, path, 0)) return;
    int error = 0;
    struct json_object *req = ix_record(root, owner, path, &error);
    if (!req) {
        if (error != -ENOENT && error != -EINVAL) mc_log(MC_LOG_WARNING, "%s could not be read: %s", path, strerror(-error));
        return;
    }
    char id[256] = "", message[256] = "";
    if (ix_deposit(client, req, id, sizeof id, message, sizeof message) == 0) mc_log(MC_LOG_INFO, "recorded %s as %s", ix_relative(root, path), id);
    else mc_log(MC_LOG_WARNING, "%s was not recorded: %s", path, message);
    json_object_put(req);
}

int main(int argc, char **argv) {
    const char *root = getenv("HOME"), *socket_path = NULL;
    bool once = false, watch = true;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) root = argv[++i];
        else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) socket_path = argv[++i];
        else if (strcmp(argv[i], "--once") == 0) once = true;
        else if (strcmp(argv[i], "--no-watch") == 0) watch = false;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { usage(stdout); return 0; }
        else { usage(stderr); return 2; }
    }
    if (!root || !*root) {
        struct passwd *pw = getpwuid(getuid());
        root = pw ? pw->pw_dir : NULL;
    }
    if (!root || !*root) {
        fprintf(stderr, "indexd: no home to index (set HOME or --root)\n");
        return 1;
    }
    mc_log_init("indexd");
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    char owner[64];
    mc_user_name(getuid(), owner, sizeof owner);
    ix_client client = { socket_path };
    mc_log(MC_LOG_NOTICE, "indexing %s for %s through %s", root, owner, ix_client_socket(&client));
    int rc = reconcile(&client, root, owner);
    if (once) return rc ? 1 : 0;

    ix_watch *w = NULL;
    if (watch) {
        int error = 0;
        w = ix_watch_open(root, &error);
        if (!w) mc_log(MC_LOG_WARNING, "no watch on %s (%s): reconciling on a timer only", root, strerror(-error));
        else mc_log(MC_LOG_INFO, "watching %zu directories", ix_watch_count(w));
    }
    int64_t last_reconcile = mc_now_ms();
    int reconciled = rc == 0;
    while (!stopping) {
        /* threadd comes up beside the desktop: a first reconcile it could not take is tried again soon */
        if (!reconciled && mc_now_ms() - last_reconcile >= 15000) {
            reconciled = reconcile(&client, root, owner) == 0;
            last_reconcile = mc_now_ms();
        }
        int64_t now = mc_now_ms();
        int wait = 1000;
        if (w) {
            int64_t due = ix_watch_due_ms(w);
            if (due >= 0) wait = due > now ? (int)(due - now) : 0;
            if (wait > 1000) wait = 1000;
            struct pollfd p = { ix_watch_fd(w), POLLIN, 0 };
            int r = poll(&p, 1, wait);
            if (r < 0 && errno != EINTR) {
                mc_log(MC_LOG_ERROR, "poll: %s", strerror(errno));
                sleep(1);
            }
            now = mc_now_ms();
            if (r > 0) ix_watch_read(w, now);
            ix_event ev;
            bool tree = false;
            while (ix_watch_next(w, now, &ev)) {
                switch (ev.kind) {
                case IX_EVENT_CHANGED:
                    record(&client, root, owner, ev.path);
                    break;
                case IX_EVENT_REMOVED: {
                    bool removed = false;
                    char message[256] = "";
                    if (ix_file_remove(&client, ev.path, &removed, message, sizeof message) == 0) {
                        if (removed) mc_log(MC_LOG_INFO, "forgot %s", ix_relative(root, ev.path));
                    } else {
                        mc_log(MC_LOG_WARNING, "%s could not be forgotten: %s", ev.path, message);
                    }
                    break;
                }
                case IX_EVENT_MOVED: {
                    char message[256] = "";
                    if (!ix_walk_admits(root, ev.path, 0)) {
                        bool removed = false;
                        ix_file_remove(&client, ev.from, &removed, message, sizeof message);   /* moved somewhere never recorded */
                    } else if (ix_file_move(&client, ev.from, ev.path, message, sizeof message) == 0) {
                        mc_log(MC_LOG_INFO, "moved %s to %s", ix_relative(root, ev.from), ix_relative(root, ev.path));
                    } else {
                        record(&client, root, owner, ev.path);   /* no record to move: make one */
                    }
                    break;
                }
                case IX_EVENT_TREE:
                    tree = true;
                    break;
                }
            }
            if (tree) {
                reconcile(&client, root, owner);
                last_reconcile = mc_now_ms();
            }
        } else {
            sleep(1);
        }
        if (mc_now_ms() - last_reconcile >= RECONCILE_EVERY_MS) {
            reconcile(&client, root, owner);
            last_reconcile = mc_now_ms();
        }
    }
    ix_watch_close(w);
    mc_log(MC_LOG_NOTICE, "stopping");
    return 0;
}
