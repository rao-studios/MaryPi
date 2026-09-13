#include <errno.h>
#include <poll.h>
#include <unistd.h>

#include "common/io.h"
#include "common/json.h"
#include "mary_test.h"
#include "runtime/ears.h"
#include "runtime/queue.h"

MARY_TEST(what_the_ears_heard_becomes_a_request_or_not) {
    struct {
        const char *text;
        bool woke;
        mr_heard_kind kind;
        const char *request;
    } rows[] = {
        { "Hey Mary, what time is it?", true, MR_HEARD_REQUEST, "what time is it?" },
        { "hey mary", true, MR_HEARD_NOTHING, "" },
        { "what time is it", true, MR_HEARD_REQUEST, "what time is it" },     /* the spotter heard the name; Voxtral lost it */
        { "stop listening", true, MR_HEARD_STOP, "" },
        { "Mary, stop listening", false, MR_HEARD_STOP, "" },
        { "   ", false, MR_HEARD_NOTHING, "" },
        { "", true, MR_HEARD_NOTHING, "" },
        { "  open my email ", false, MR_HEARD_REQUEST, "open my email" },
        { "tell mary I said hi", false, MR_HEARD_REQUEST, "tell mary I said hi" },
    };
    for (size_t i = 0; i < sizeof rows / sizeof *rows; i++) {
        char request[128];
        mr_heard_kind kind = mr_heard(rows[i].text, rows[i].woke, request, sizeof request);
        if (kind != rows[i].kind) MARY_FAIL("\"%s\": kind %d, expected %d", rows[i].text, (int)kind, (int)rows[i].kind);
        else if (kind == MR_HEARD_REQUEST && strcmp(request, rows[i].request) != 0)
            MARY_FAIL("\"%s\": request \"%s\", expected \"%s\"", rows[i].text, request, rows[i].request);
    }
}

MARY_TEST(the_queue_hands_events_over_in_order_and_wakes_the_loop) {
    mr_queue q;
    MARY_ASSERT_EQ(mr_queue_init(&q), 0);
    struct pollfd p = { .fd = mr_queue_fd(&q), .events = POLLIN };
    MARY_ASSERT_EQ(poll(&p, 1, 0), 0);
    for (int i = 0; i < 100; i++) {
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "n", json_object_new_int(i));
        MARY_ASSERT_EQ(mr_queue_push(&q, o), 0);
    }
    MARY_ASSERT_EQ(poll(&p, 1, 0), 1);
    mr_queue_drain(&q);
    MARY_ASSERT_EQ(poll(&p, 1, 0), 0);
    for (int i = 0; i < 100; i++) {
        struct json_object *o = mr_queue_pop(&q);
        int64_t n = -1;
        MARY_ASSERT(o && mc_json_int64(o, "n", &n) && n == i);
        json_object_put(o);
    }
    MARY_ASSERT(mr_queue_pop(&q) == NULL);
    json_object_put(NULL);
    mr_queue_push(&q, json_object_new_object());    /* released by free */
    mr_queue_free(&q);
}

MARY_TEST(a_session_without_sewnd_is_an_error_and_standby_again) {
    mr_queue q;
    mr_queue_init(&q);
    mr_ears_config config = { .sewn_socket = "/tmp/no-such-dir/sewn.sock", .wake = true, .listen_timeout_ms = 300 };
    int error = 0;
    mr_ears *ears = mr_ears_new(&config, &q, &error);
    MARY_ASSERT(ears != NULL);
    if (!ears) return;
    MARY_ASSERT(!mr_ears_can_wake(ears));          /* no spotter was given */
    mr_ears_listen(ears, false);
    struct json_object *event = NULL;
    for (int i = 0; i < 100 && !event; i++) {
        usleep(10000);
        event = mr_queue_pop(&q);
    }
    MARY_ASSERT(event != NULL);
    MARY_ASSERT_STR(mc_json_type(event), "ears.error");
    MARY_ASSERT_STR(mc_json_string(event, "stage"), "sewnd");
    json_object_put(event);
    mr_ears_free(ears);
    mr_queue_free(&q);
}

int main(void) {
    mc_ignore_sigpipe();
    MARY_RUN(what_the_ears_heard_becomes_a_request_or_not);
    MARY_RUN(the_queue_hands_events_over_in_order_and_wakes_the_loop);
    MARY_RUN(a_session_without_sewnd_is_an_error_and_standby_again);
    MARY_TEST_MAIN_END();
}
