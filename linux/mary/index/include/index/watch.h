/* The home, watched: a recursive inotify over every directory the walk admits, with
 * events settled per path for IX_WATCH_SETTLE_MS so a save that writes three times
 * is one change. On other systems the watch does not exist (-ENOSYS) and indexd
 * reconciles on a timer. */
#ifndef MARY_INDEX_WATCH_H
#define MARY_INDEX_WATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IX_WATCH_SETTLE_MS 500

typedef enum ix_event_kind {
    IX_EVENT_CHANGED,       /* created or written: (re)record path */
    IX_EVENT_REMOVED,       /* deleted, or moved away: remove path's record */
    IX_EVENT_MOVED,         /* renamed within the home: from → path */
    IX_EVENT_TREE,          /* a directory appeared or moved: reconcile */
} ix_event_kind;

typedef struct ix_event {
    ix_event_kind kind;
    char path[4096];
    char from[4096];        /* MOVED */
} ix_event;

typedef struct ix_watch ix_watch;

/* NULL with *error (-ENOSYS where there is no inotify). */
ix_watch *ix_watch_open(const char *root, int *error);
void ix_watch_close(ix_watch *w);
/* The fd to poll for readability. */
int ix_watch_fd(const ix_watch *w);
/* Reads whatever inotify has (non-blocking) into the pending set; `now_ms` is monotonic. */
int ix_watch_read(ix_watch *w, int64_t now_ms);
/* Pops one settled event (older than IX_WATCH_SETTLE_MS at now_ms). True when one was written. */
bool ix_watch_next(ix_watch *w, int64_t now_ms, ix_event *out);
/* When the next pending event settles, or -1 when none is pending. */
int64_t ix_watch_due_ms(const ix_watch *w);
/* How many directories are watched (tests). */
size_t ix_watch_count(const ix_watch *w);

#endif
