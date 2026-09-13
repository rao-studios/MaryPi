/* The inotify watch (Linux). Elsewhere the watch reports ENOSYS and that is all
 * that is checked. */
#include "test_support.h"

#include <poll.h>
#include <time.h>

#include "common/io.h"
#include "index/watch.h"

static void pause_ms(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Reads and settles until one event arrives or `wait_ms` pass. */
static bool next_event(ix_watch *w, ix_event *ev, int wait_ms) {
    int64_t until = mc_now_ms() + wait_ms;
    for (;;) {
        int64_t now = mc_now_ms();
        if (now >= until) return false;
        struct pollfd p = { ix_watch_fd(w), POLLIN, 0 };
        poll(&p, 1, 50);
        now = mc_now_ms();
        ix_watch_read(w, now);
        if (ix_watch_next(w, now, ev)) return true;
    }
}

MARY_TEST(a_save_a_rename_a_delete_and_a_new_folder_are_one_event_each) {
    make_home();
    put_file("Documents/keep.txt", "k");
    int error = 0;
    ix_watch *w = ix_watch_open(home, &error);
#if !defined(__linux__)
    MARY_ASSERT(w == NULL);
    MARY_ASSERT_EQ(error, -ENOSYS);
    remove_tree(home);
    return;
#endif
    MARY_ASSERT(w != NULL);
    MARY_ASSERT_EQ(ix_watch_count(w), 2);          /* the home and Documents */
    ix_event ev;
    /* a save that writes several times settles into one change */
    for (int i = 0; i < 3; i++) {
        put_file("Documents/notes.txt", "draft");
        pause_ms(50);
    }
    MARY_ASSERT(next_event(w, &ev, 3000));
    MARY_ASSERT_EQ(ev.kind, IX_EVENT_CHANGED);
    MARY_ASSERT(strstr(ev.path, "Documents/notes.txt") != NULL);
    MARY_ASSERT(!next_event(w, &ev, 700));
    /* a rename within the home is a move */
    char from[512], to[512];
    snprintf(from, sizeof from, "%s/Documents/notes.txt", home);
    snprintf(to, sizeof to, "%s/Documents/final.txt", home);
    MARY_ASSERT_EQ(rename(from, to), 0);
    MARY_ASSERT(next_event(w, &ev, 3000));
    MARY_ASSERT_EQ(ev.kind, IX_EVENT_MOVED);
    MARY_ASSERT_STR(ev.from, from);
    MARY_ASSERT_STR(ev.path, to);
    /* a delete */
    MARY_ASSERT_EQ(unlink(to), 0);
    MARY_ASSERT(next_event(w, &ev, 3000));
    MARY_ASSERT_EQ(ev.kind, IX_EVENT_REMOVED);
    MARY_ASSERT_STR(ev.path, to);
    /* a new folder is watched: a file inside it is seen */
    put_file("Projects/new/idea.md", "i");
    bool tree = false, changed = false;
    while (next_event(w, &ev, 3000)) {
        if (ev.kind == IX_EVENT_TREE) tree = true;
        if (ev.kind == IX_EVENT_CHANGED && strstr(ev.path, "idea.md")) changed = true;
        if (tree && changed) break;
    }
    MARY_ASSERT(tree);
    MARY_ASSERT(ix_watch_count(w) >= 4);
    /* dotted names never surface */
    put_file(".hidden.txt", "h");
    MARY_ASSERT(!next_event(w, &ev, 700) || !strstr(ev.path, ".hidden"));
    ix_watch_close(w);
    remove_tree(home);
}

int main(void) {
    MARY_RUN(a_save_a_rename_a_delete_and_a_new_folder_are_one_event_each);
    MARY_TEST_MAIN_END();
}
