#include <errno.h>

#include "common/sse.h"
#include "mary_test.h"

struct events {
    char event[8][64];
    char data[8][256];
    int count;
    int stop_after;
};

static int collect(const char *event, const char *data, size_t len, void *user) {
    struct events *e = user;
    if (e->count < 8) {
        snprintf(e->event[e->count], sizeof e->event[0], "%s", event);
        snprintf(e->data[e->count], sizeof e->data[0], "%.*s", (int)len, data);
    }
    e->count++;
    return e->stop_after && e->count >= e->stop_after;
}

static void feed_bytewise(mc_sse_parser *p, const char *s, struct events *e) {
    for (; *s; s++) mc_sse_feed(p, s, 1, collect, e);
}

MARY_TEST(mistral_chat_chunks_and_done_arrive_byte_by_byte) {
    mc_sse_parser p;
    mc_sse_init(&p, 4096);
    struct events e = { 0 };
    feed_bytewise(&p, "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n"
                      "data: {\"choices\":[{\"delta\":{\"content\":\" there\"}}]}\n\n"
                      "data: [DONE]\n\n", &e);
    MARY_ASSERT_EQ(e.count, 3);
    MARY_ASSERT_STR(e.event[0], "");
    MARY_ASSERT_STR(e.data[0], "{\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}");
    MARY_ASSERT_STR(e.data[2], "[DONE]");
    mc_sse_free(&p);
}

MARY_TEST(crlf_split_across_reads_still_ends_the_line) {
    mc_sse_parser p;
    mc_sse_init(&p, 4096);
    struct events e = { 0 };
    MARY_ASSERT_EQ(mc_sse_feed(&p, "data: x\r", 8, collect, &e), 0);
    MARY_ASSERT_EQ(mc_sse_feed(&p, "\n\r", 2, collect, &e), 0);
    MARY_ASSERT_EQ(e.count, 0);
    MARY_ASSERT_EQ(mc_sse_feed(&p, "\n", 1, collect, &e), 0);
    MARY_ASSERT_EQ(e.count, 1);
    MARY_ASSERT_STR(e.data[0], "x");
    mc_sse_free(&p);
}

MARY_TEST(named_events_join_their_data_lines) {
    mc_sse_parser p;
    mc_sse_init(&p, 4096);
    struct events e = { 0 };
    const char *s = ": keep-alive\n\nevent: speech.audio.delta\ndata: one\ndata:two\ndata:  three\nid: 7\n\n";
    MARY_ASSERT_EQ(mc_sse_feed(&p, s, strlen(s), collect, &e), 0);
    MARY_ASSERT_EQ(e.count, 1);   /* the comment's blank line has no data: nothing dispatched */
    MARY_ASSERT_STR(e.event[0], "speech.audio.delta");
    MARY_ASSERT_STR(e.data[0], "one\ntwo\n three");
    mc_sse_free(&p);
}

MARY_TEST(a_stream_ending_without_a_blank_line_is_finished) {
    mc_sse_parser p;
    mc_sse_init(&p, 4096);
    struct events e = { 0 };
    MARY_ASSERT_EQ(mc_sse_feed(&p, "data: whole\n\ndata: tail", 23, collect, &e), 0);
    MARY_ASSERT_EQ(e.count, 1);
    MARY_ASSERT_EQ(mc_sse_finish(&p, collect, &e), 0);
    MARY_ASSERT_EQ(e.count, 2);
    MARY_ASSERT_STR(e.data[1], "tail");
    MARY_ASSERT_EQ(mc_sse_finish(&p, collect, &e), 0);   /* nothing left */
    MARY_ASSERT_EQ(e.count, 2);
    mc_sse_free(&p);
}

MARY_TEST(stopping_and_oversize_events_are_reported) {
    mc_sse_parser p;
    mc_sse_init(&p, 32);
    struct events e = { .stop_after = 1 };
    MARY_ASSERT_EQ(mc_sse_feed(&p, "data: a\n\ndata: b\n\n", 18, collect, &e), 1);
    MARY_ASSERT_EQ(e.count, 1);
    mc_sse_free(&p);
    mc_sse_init(&p, 32);
    struct events big = { 0 };
    MARY_ASSERT_EQ(mc_sse_feed(&p, "data: 0123456789012345678901234567890123456789\n\n", 48, collect, &big), -EMSGSIZE);
    MARY_ASSERT_EQ(big.count, 0);
    mc_sse_free(&p);
}

int main(void) {
    MARY_RUN(mistral_chat_chunks_and_done_arrive_byte_by_byte);
    MARY_RUN(crlf_split_across_reads_still_ends_the_line);
    MARY_RUN(named_events_join_their_data_lines);
    MARY_RUN(a_stream_ending_without_a_blank_line_is_finished);
    MARY_RUN(stopping_and_oversize_events_are_reported);
    MARY_TEST_MAIN_END();
}
