#include "index/watch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "index/walk.h"

/* One pending change per path, settled after IX_WATCH_SETTLE_MS of quiet. */
struct pending {
    ix_event event;
    int64_t at_ms;
};

struct ix_watch {
    int fd;
    char root[4096];
    struct { int wd; char *path; } *dirs;
    size_t n_dirs, cap_dirs;
    struct pending *pending;
    size_t n_pending, cap_pending;
    /* a rename's first half, waiting for its second */
    uint32_t move_cookie;
    char move_from[4096];
    bool move_is_dir;
    int64_t move_at_ms;
};

static struct pending *pend(ix_watch *w, ix_event_kind kind, const char *path, const char *from, int64_t now_ms) {
    /* the newest event on a path replaces the older one; a move keeps its `from` */
    for (size_t i = 0; i < w->n_pending; i++) {
        if (strcmp(w->pending[i].event.path, path) == 0) {
            if (!(w->pending[i].event.kind == IX_EVENT_MOVED && kind == IX_EVENT_CHANGED)) w->pending[i].event.kind = kind;
            if (from) snprintf(w->pending[i].event.from, sizeof w->pending[i].event.from, "%s", from);
            w->pending[i].at_ms = now_ms;
            return &w->pending[i];
        }
    }
    if (w->n_pending == w->cap_pending) {
        w->cap_pending = w->cap_pending ? w->cap_pending * 2 : 32;
        w->pending = realloc(w->pending, w->cap_pending * sizeof *w->pending);
    }
    struct pending *p = &w->pending[w->n_pending++];
    memset(p, 0, sizeof *p);
    p->event.kind = kind;
    snprintf(p->event.path, sizeof p->event.path, "%s", path);
    if (from) snprintf(p->event.from, sizeof p->event.from, "%s", from);
    p->at_ms = now_ms;
    return p;
}

int64_t ix_watch_due_ms(const ix_watch *w) {
    int64_t due = -1;
    for (size_t i = 0; i < w->n_pending; i++) {
        int64_t at = w->pending[i].at_ms + IX_WATCH_SETTLE_MS;
        if (due < 0 || at < due) due = at;
    }
    if (w->move_cookie) {
        int64_t at = w->move_at_ms + IX_WATCH_SETTLE_MS;
        if (due < 0 || at < due) due = at;
    }
    return due;
}

bool ix_watch_next(ix_watch *w, int64_t now_ms, ix_event *out) {
    /* a rename whose second half never came was a move out of the home: a removal */
    if (w->move_cookie && now_ms - w->move_at_ms >= IX_WATCH_SETTLE_MS) {
        pend(w, w->move_is_dir ? IX_EVENT_TREE : IX_EVENT_REMOVED, w->move_from, NULL, w->move_at_ms);
        w->move_cookie = 0;
    }
    for (size_t i = 0; i < w->n_pending; i++) {
        if (now_ms - w->pending[i].at_ms < IX_WATCH_SETTLE_MS) continue;
        *out = w->pending[i].event;
        memmove(&w->pending[i], &w->pending[i + 1], (w->n_pending - i - 1) * sizeof *w->pending);
        w->n_pending--;
        return true;
    }
    return false;
}

size_t ix_watch_count(const ix_watch *w) { return w->n_dirs; }
int ix_watch_fd(const ix_watch *w) { return w->fd; }

#if defined(__linux__)
#include <dirent.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define MASK (IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_CREATE | IN_DELETE_SELF | IN_ONLYDIR | IN_EXCL_UNLINK)

static const char *dir_of(const ix_watch *w, int wd) {
    for (size_t i = 0; i < w->n_dirs; i++) if (w->dirs[i].wd == wd) return w->dirs[i].path;
    return NULL;
}

static void forget(ix_watch *w, int wd) {
    for (size_t i = 0; i < w->n_dirs; i++) {
        if (w->dirs[i].wd != wd) continue;
        free(w->dirs[i].path);
        w->dirs[i] = w->dirs[--w->n_dirs];
        return;
    }
}

/* Watches `dir` and, recursively, the directories under it that the walk admits. */
static int add_tree(ix_watch *w, const char *dir) {
    int wd = inotify_add_watch(w->fd, dir, MASK);
    if (wd < 0) return -errno;
    bool known = false;
    for (size_t i = 0; i < w->n_dirs; i++) if (w->dirs[i].wd == wd) known = true;
    if (!known) {
        if (w->n_dirs == w->cap_dirs) {
            w->cap_dirs = w->cap_dirs ? w->cap_dirs * 2 : 64;
            w->dirs = realloc(w->dirs, w->cap_dirs * sizeof *w->dirs);
        }
        w->dirs[w->n_dirs].wd = wd;
        w->dirs[w->n_dirs].path = strdup(dir);
        w->n_dirs++;
    }
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) add_tree(w, path);
    }
    closedir(d);
    return 0;
}

ix_watch *ix_watch_open(const char *root, int *error) {
    ix_watch *w = calloc(1, sizeof *w);
    if (!w) {
        *error = -ENOMEM;
        return NULL;
    }
    snprintf(w->root, sizeof w->root, "%s", root);
    w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (w->fd < 0) {
        *error = -errno;
        free(w);
        return NULL;
    }
    int rc = add_tree(w, root);
    if (rc) {
        *error = rc;
        ix_watch_close(w);
        return NULL;
    }
    *error = 0;
    return w;
}

void ix_watch_close(ix_watch *w) {
    if (!w) return;
    for (size_t i = 0; i < w->n_dirs; i++) free(w->dirs[i].path);
    free(w->dirs);
    free(w->pending);
    if (w->fd >= 0) close(w->fd);
    free(w);
}

int ix_watch_read(ix_watch *w, int64_t now_ms) {
    char buf[64 * 1024] __attribute__((aligned(8)));
    for (;;) {
        ssize_t n = read(w->fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return errno == EAGAIN ? 0 : -errno;
        }
        if (n == 0) return 0;
        for (char *p = buf; p < buf + n;) {
            struct inotify_event *ev = (struct inotify_event *)p;
            p += sizeof *ev + ev->len;
            if (ev->mask & IN_Q_OVERFLOW) {
                pend(w, IX_EVENT_TREE, w->root, NULL, now_ms);
                continue;
            }
            const char *dir = dir_of(w, ev->wd);
            if (!dir) continue;
            if (ev->mask & IN_IGNORED) {
                forget(w, ev->wd);
                continue;
            }
            if (ev->mask & IN_DELETE_SELF) {
                forget(w, ev->wd);
                continue;
            }
            if (!ev->len || ev->name[0] == '.') continue;   /* dotted names are never recorded */
            char path[4096];
            snprintf(path, sizeof path, "%s/%s", dir, ev->name);
            bool is_dir = (ev->mask & IN_ISDIR) != 0;
            if (ev->mask & IN_MOVED_FROM) {
                /* a rename's first half: keep it until its MOVED_TO or the settle time */
                if (w->move_cookie) pend(w, w->move_is_dir ? IX_EVENT_TREE : IX_EVENT_REMOVED, w->move_from, NULL, w->move_at_ms);
                w->move_cookie = ev->cookie;
                snprintf(w->move_from, sizeof w->move_from, "%s", path);
                w->move_is_dir = is_dir;
                w->move_at_ms = now_ms;
                continue;
            }
            if (ev->mask & IN_MOVED_TO) {
                if (w->move_cookie && ev->cookie == w->move_cookie) {
                    if (is_dir) {
                        add_tree(w, path);
                        pend(w, IX_EVENT_TREE, path, w->move_from, now_ms);
                    } else {
                        pend(w, IX_EVENT_MOVED, path, w->move_from, now_ms);
                    }
                    w->move_cookie = 0;
                } else if (is_dir) {
                    add_tree(w, path);
                    pend(w, IX_EVENT_TREE, path, NULL, now_ms);
                } else {
                    pend(w, IX_EVENT_CHANGED, path, NULL, now_ms);
                }
                continue;
            }
            if (is_dir) {
                if (ev->mask & IN_CREATE) {
                    add_tree(w, path);
                    pend(w, IX_EVENT_TREE, path, NULL, now_ms);
                }
                continue;
            }
            if (ev->mask & IN_DELETE) pend(w, IX_EVENT_REMOVED, path, NULL, now_ms);
            else if (ev->mask & (IN_CLOSE_WRITE | IN_CREATE)) pend(w, IX_EVENT_CHANGED, path, NULL, now_ms);
        }
    }
}

#else

ix_watch *ix_watch_open(const char *root, int *error) {
    (void)root;
    *error = -ENOSYS;
    return NULL;
}
void ix_watch_close(ix_watch *w) { free(w); }
int ix_watch_read(ix_watch *w, int64_t now_ms) {
    (void)w;
    (void)now_ms;
    return -ENOSYS;
}

#endif
