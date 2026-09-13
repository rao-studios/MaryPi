#include "test_support.h"

#include "index/kind.h"

MARY_TEST(kinds_follow_the_finder) {
    make_home();
    put_file("a.txt", "hello");
    put_file("b.c", "int x;");
    put_file("c.png", "\x89PNG");
    put_file("d.mp3", "ID3");
    put_file("e.mp4", "x");
    put_file("f.pdf", "%PDF");
    put_file("g", "plain words with no extension");
    put_file("h.bin", "bytes\0here");
    put_file("empty", "");
    char path[512];
#define P(name) (snprintf(path, sizeof path, "%s/%s", home, name), path)
    MARY_ASSERT_STR(ix_kind_of(P("a.txt")), "text");
    MARY_ASSERT_STR(ix_kind_of(P("b.c")), "code");
    MARY_ASSERT_STR(ix_kind_of(P("c.png")), "image");
    MARY_ASSERT_STR(ix_kind_of(P("d.mp3")), "audio");
    MARY_ASSERT_STR(ix_kind_of(P("e.mp4")), "video");
    MARY_ASSERT_STR(ix_kind_of(P("f.pdf")), "pdf");
    MARY_ASSERT_STR(ix_kind_of(P("g")), "text");                /* sniffed */
    MARY_ASSERT_STR(ix_kind_of(P("empty")), "text");
    MARY_ASSERT_STR(ix_kind_of(P("nothing.xyz")), "document");    /* unreadable: not text */
    MARY_ASSERT(ix_is_text(P("b.c")));
    MARY_ASSERT(!ix_is_text(P("c.png")));
    put_file("h.bin", "bytes");
    FILE *f = fopen(P("h.bin"), "wb");
    fwrite("by\0tes", 1, 6, f);
    fclose(f);
    MARY_ASSERT_STR(ix_kind_of(P("h.bin")), "document");        /* a NUL: not text */
    MARY_ASSERT_STR(ix_basename("/a/b/c.txt"), "c.txt");
    MARY_ASSERT_STR(ix_basename("c.txt"), "c.txt");
    remove_tree(home);
}

int main(void) {
    MARY_RUN(kinds_follow_the_finder);
    MARY_TEST_MAIN_END();
}
