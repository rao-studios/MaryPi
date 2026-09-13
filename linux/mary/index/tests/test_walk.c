#include "test_support.h"

#include "index/walk.h"

static char seen[16][256];
static int n_seen;

static int collect(const char *path, int64_t size, int64_t mtime_ms, void *user) {
    if (n_seen < 16) snprintf(seen[n_seen++], 256, "%s", path + strlen(home) + 1);
    MARY_ASSERT(mtime_ms > 0);
    (void)size;
    return 0;
}

MARY_TEST(the_walk_skips_dots_symlinks_and_the_trash_and_orders_names) {
    make_home();
    put_file("Documents/notes.txt", "n");
    put_file("Documents/b/deep.md", "d");
    put_file("Documents/a.txt", "a");
    put_file(".config/maryui/settings.conf", "x");
    put_file(".local/share/Trash/files/gone.txt", "x");
    put_file("Pictures/.hidden.png", "x");
    put_file("Music/song.mp3", "x");
    char link[512], target[512];
    snprintf(target, sizeof target, "%s/Music/song.mp3", home);
    snprintf(link, sizeof link, "%s/Music/alias.mp3", home);
    MARY_ASSERT_EQ(symlink(target, link), 0);
    n_seen = 0;
    long n = ix_walk(home, collect, NULL);
    MARY_ASSERT_EQ(n, 4);
    MARY_ASSERT_STR(seen[0], "Documents/a.txt");
    MARY_ASSERT_STR(seen[1], "Documents/b/deep.md");
    MARY_ASSERT_STR(seen[2], "Documents/notes.txt");
    MARY_ASSERT_STR(seen[3], "Music/song.mp3");
    char path[512];
    snprintf(path, sizeof path, "%s/Documents/a.txt", home);
    MARY_ASSERT(ix_walk_admits(home, path, 10));
    MARY_ASSERT(!ix_walk_admits(home, path, (int64_t)IX_FILE_MAX + 1));
    snprintf(path, sizeof path, "%s/.config/x", home);
    MARY_ASSERT(!ix_walk_admits(home, path, 1));
    snprintf(path, sizeof path, "%s/Documents/.draft.txt", home);
    MARY_ASSERT(!ix_walk_admits(home, path, 1));
    MARY_ASSERT(!ix_walk_admits(home, "/etc/passwd", 1));
    MARY_ASSERT(ix_walk("/nonexistent/dir", collect, NULL) < 0);
    remove_tree(home);
}

int main(void) {
    MARY_RUN(the_walk_skips_dots_symlinks_and_the_trash_and_orders_names);
    MARY_TEST_MAIN_END();
}
