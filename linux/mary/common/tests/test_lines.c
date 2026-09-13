#include <errno.h>
#include "common/buf.h"
#include "common/lines.h"
#include "mary_test.h"

struct collected {
    char lines[8][64];
    int count;
    int stop_after;
};

static int collect(const char *line, size_t len, void *user) {
    struct collected *c = user;
    if (c->count < 8) snprintf(c->lines[c->count], sizeof c->lines[0], "%.*s", (int)len, line);
    c->count++;
    return c->stop_after && c->count >= c->stop_after;
}

MARY_TEST(a_buffer_stays_terminated_as_it_grows_and_shrinks) {
    mc_buf b = { 0 };
    for (int i = 0; i < 100; i++) mc_buf_append_str(&b, "abcdefghij");
    MARY_ASSERT_EQ(b.len, 1000);
    MARY_ASSERT_EQ(b.data[b.len], 0);
    mc_buf_consume(&b, 995);
    MARY_ASSERT_STR((char *)b.data, "fghij");
    mc_buf_consume(&b, 99);
    MARY_ASSERT_EQ(b.len, 0);
    MARY_ASSERT_STR((char *)b.data, "");
    mc_buf_free_secure(&b);
    MARY_ASSERT(b.data == NULL);
}

MARY_TEST(lines_arrive_whole_however_the_bytes_are_split) {
    const char *stream = "{\"type\":\"wake\"}\n{\"type\":\"state\",\"state\":\"listening\"}\r\n\n";
    mc_line_reader r;
    mc_line_reader_init(&r, 1024);
    struct collected c = { 0 };
    for (const char *p = stream; *p; p++) MARY_ASSERT_EQ(mc_line_reader_feed(&r, p, 1, collect, &c), 0);
    MARY_ASSERT_EQ(c.count, 3);
    MARY_ASSERT_STR(c.lines[0], "{\"type\":\"wake\"}");
    MARY_ASSERT_STR(c.lines[1], "{\"type\":\"state\",\"state\":\"listening\"}");
    MARY_ASSERT_STR(c.lines[2], "");
    mc_line_reader_free(&r);
}

MARY_TEST(an_oversize_line_is_dropped_and_reading_carries_on) {
    mc_line_reader r;
    mc_line_reader_init(&r, 8);
    struct collected c = { 0 };
    MARY_ASSERT_EQ(mc_line_reader_feed(&r, "ok\n0123456789", 13, collect, &c), -EMSGSIZE);
    MARY_ASSERT_EQ(mc_line_reader_feed(&r, "more-still\nnext\n", 16, collect, &c), 0);
    MARY_ASSERT_EQ(c.count, 2);
    MARY_ASSERT_STR(c.lines[0], "ok");
    MARY_ASSERT_STR(c.lines[1], "next");
    mc_line_reader_free(&r);
}

MARY_TEST(a_callback_can_stop_the_reader) {
    mc_line_reader r;
    mc_line_reader_init(&r, 64);
    struct collected c = { .stop_after = 1 };
    MARY_ASSERT_EQ(mc_line_reader_feed(&r, "a\nb\nc\n", 6, collect, &c), 1);
    MARY_ASSERT_EQ(c.count, 1);
    mc_line_reader_free(&r);
}

int main(void) {
    MARY_RUN(a_buffer_stays_terminated_as_it_grows_and_shrinks);
    MARY_RUN(lines_arrive_whole_however_the_bytes_are_split);
    MARY_RUN(an_oversize_line_is_dropped_and_reading_carries_on);
    MARY_RUN(a_callback_can_stop_the_reader);
    MARY_TEST_MAIN_END();
}
