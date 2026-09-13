#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/frame.h"
#include "common/io.h"
#include "common/json.h"
#include "mary_test.h"

struct frames {
    uint8_t kind[8];
    char payload[8][128];
    size_t len[8];
    int count;
    int stop_after;
};

static int collect(uint8_t kind, const unsigned char *payload, size_t len, void *user) {
    struct frames *f = user;
    if (f->count < 8) {
        f->kind[f->count] = kind;
        f->len[f->count] = len;
        memcpy(f->payload[f->count], payload, len < 127 ? len : 127);
        f->payload[f->count][len < 127 ? len : 127] = 0;
    }
    f->count++;
    return f->stop_after && f->count >= f->stop_after;
}

MARY_TEST(frames_reassemble_from_single_bytes) {
    mc_buf wire = { 0 };
    MARY_ASSERT_EQ(mc_frame_append(&wire, MC_FRAME_JSON, (const unsigned char *)"{\"type\":\"turn.end\"}", 19), 0);
    const unsigned char pcm[] = { 0, 0, 128, 63, 0, 0, 0, 0 };
    MARY_ASSERT_EQ(mc_frame_append(&wire, MC_FRAME_PCM, pcm, sizeof pcm), 0);
    MARY_ASSERT_EQ(mc_frame_append(&wire, MC_FRAME_JSON, NULL, 0), 0);
    MARY_ASSERT_EQ(wire.len, 3 * MC_FRAME_HEADER + 19 + sizeof pcm);
    MARY_ASSERT_EQ(wire.data[0], 19);   /* little-endian length first */
    MARY_ASSERT_EQ(wire.data[4], MC_FRAME_JSON);

    mc_frame_reader r;
    mc_frame_reader_init(&r, 0, true);
    struct frames f = { 0 };
    for (size_t i = 0; i < wire.len; i++) MARY_ASSERT_EQ(mc_frame_reader_feed(&r, wire.data + i, 1, collect, &f), 0);
    MARY_ASSERT_EQ(f.count, 3);
    MARY_ASSERT_STR(f.payload[0], "{\"type\":\"turn.end\"}");
    MARY_ASSERT_EQ(f.kind[1], MC_FRAME_PCM);
    MARY_ASSERT_EQ(f.len[1], sizeof pcm);
    MARY_ASSERT(memcmp(f.payload[1], pcm, sizeof pcm) == 0);
    MARY_ASSERT_EQ(f.len[2], 0);
    MARY_ASSERT_EQ(r.buf.len, 0);
    mc_frame_reader_free(&r);
    mc_buf_free(&wire);
}

MARY_TEST(an_oversize_header_is_refused_before_any_payload) {
    mc_frame_reader r;
    mc_frame_reader_init(&r, 16, false);
    struct frames f = { 0 };
    const unsigned char header[] = { 17, 0, 0, 0, MC_FRAME_JSON };
    MARY_ASSERT_EQ(mc_frame_reader_feed(&r, header, sizeof header, collect, &f), -EMSGSIZE);
    MARY_ASSERT_EQ(f.count, 0);
    MARY_ASSERT_EQ(mc_frame_append(&(mc_buf){ 0 }, MC_FRAME_PCM, NULL, MC_FRAME_MAX + 1), -EMSGSIZE);
    mc_frame_reader_free(&r);
}

MARY_TEST(frames_cross_a_socket_and_eof_is_seen) {
    int sv[2];
    MARY_ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    struct json_object *msg = json_object_new_object();
    json_object_object_add(msg, "type", json_object_new_string("token"));
    json_object_object_add(msg, "text", json_object_new_string("Paris/France"));
    MARY_ASSERT_EQ(mc_frame_write_json(sv[0], msg), 0);
    json_object_put(msg);
    const unsigned char pcm[4] = { 1, 2, 3, 4 };
    MARY_ASSERT_EQ(mc_frame_write_fd(sv[0], MC_FRAME_PCM, pcm, 4), 0);
    close(sv[0]);

    mc_frame_reader r;
    mc_frame_reader_init(&r, 0, false);
    struct frames f = { 0 };
    int status;
    while ((status = mc_frame_reader_read_fd(&r, sv[1], collect, &f)) == MC_IO_OK) {}
    MARY_ASSERT_EQ(status, MC_IO_EOF);
    MARY_ASSERT_EQ(f.count, 2);
    MARY_ASSERT_STR(f.payload[0], "{\"type\":\"token\",\"text\":\"Paris/France\"}");
    MARY_ASSERT_EQ(f.kind[1], MC_FRAME_PCM);
    close(sv[1]);
    mc_frame_reader_free(&r);
}

MARY_TEST(a_callback_can_stop_mid_stream) {
    mc_buf wire = { 0 };
    for (int i = 0; i < 3; i++) mc_frame_append(&wire, MC_FRAME_JSON, (const unsigned char *)"{}", 2);
    mc_frame_reader r;
    mc_frame_reader_init(&r, 0, false);
    struct frames f = { .stop_after = 1 };
    MARY_ASSERT_EQ(mc_frame_reader_feed(&r, wire.data, wire.len, collect, &f), 1);
    MARY_ASSERT_EQ(f.count, 1);
    MARY_ASSERT_EQ(r.buf.len, 2 * (MC_FRAME_HEADER + 2));   /* the rest waits */
    mc_frame_reader_free(&r);
    mc_buf_free(&wire);
}

int main(void) {
    MARY_RUN(frames_reassemble_from_single_bytes);
    MARY_RUN(an_oversize_header_is_refused_before_any_payload);
    MARY_RUN(frames_cross_a_socket_and_eof_is_seen);
    MARY_RUN(a_callback_can_stop_mid_stream);
    MARY_TEST_MAIN_END();
}
