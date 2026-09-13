#include <errno.h>

#include "common/io.h"
#include "common/json.h"
#include "computer-use/invoke.h"
#include "mary_test.h"

struct wire {
    char sent[8][512];
    int count;
    int fail;
};

static int send_to(struct json_object *message, void *user) {
    struct wire *w = user;
    if (w->fail) return -EPIPE;
    if (w->count < 8) snprintf(w->sent[w->count], sizeof w->sent[0], "%s", mc_json_compact(message, NULL));
    w->count++;
    return 0;
}

struct outcome {
    int calls;
    bool ok;
    char error[32];
    char result[256];
};

static void record(const mcu_result *r, void *user) {
    struct outcome *o = user;
    o->calls++;
    o->ok = r->ok;
    snprintf(o->error, sizeof o->error, "%s", r->error ? r->error : "");
    snprintf(o->result, sizeof o->result, "%s", r->result ? mc_json_compact(r->result, NULL) : "");
}

static struct json_object *result_line(const char *text) { return mc_json_parse(text, strlen(text)); }

MARY_TEST(an_invoke_goes_out_and_its_result_comes_back) {
    struct wire w = { 0 };
    mcu_pipes *p = mcu_pipes_new(send_to, &w);
    struct outcome o = { 0 };
    struct json_object *args = result_line("{\"pane\":\"sound\"}");
    char id[48];
    MARY_ASSERT_EQ(mcu_invoke(p, "settings", "open_pane", args, 5000, record, &o, id, sizeof id), 0);
    json_object_put(args);
    MARY_ASSERT_EQ(w.count, 1);
    struct json_object *sent = mc_json_parse(w.sent[0], strlen(w.sent[0]));
    MARY_ASSERT_STR(mc_json_type(sent), "skill.invoke");
    MARY_ASSERT_STR(mc_json_string(sent, "call_id"), id);
    MARY_ASSERT_STR(mc_json_string(sent, "app"), "settings");
    MARY_ASSERT_STR(mc_json_string(mc_json_object(sent, "args"), "pane"), "sound");
    json_object_put(sent);
    MARY_ASSERT_EQ(mcu_pipes_pending(p), 1);

    char line[256];
    snprintf(line, sizeof line, "{\"type\":\"skill.result\",\"call_id\":\"%s\",\"ok\":true,\"result\":{\"pane\":\"sound\"}}", id);
    struct json_object *reply = result_line(line);
    MARY_ASSERT_EQ(mcu_pipes_on_message(p, reply), 1);
    MARY_ASSERT_EQ(o.calls, 1);
    MARY_ASSERT(o.ok);
    MARY_ASSERT_STR(o.result, "{\"pane\":\"sound\"}");
    MARY_ASSERT_EQ(mcu_pipes_on_message(p, reply), -ENOENT);   /* a second result is late */
    json_object_put(reply);
    struct json_object *other = result_line("{\"type\":\"state\",\"state\":\"idle\"}");
    MARY_ASSERT_EQ(mcu_pipes_on_message(p, other), 0);
    json_object_put(other);
    mcu_pipes_free(p);
}

MARY_TEST(results_match_their_calls_in_any_order) {
    struct wire w = { 0 };
    mcu_pipes *p = mcu_pipes_new(send_to, &w);
    struct outcome first = { 0 }, second = { 0 };
    char id1[48], id2[48], line[256];
    mcu_invoke(p, "calendar", "events_today", NULL, 5000, record, &first, id1, sizeof id1);
    mcu_invoke(p, "media", "play_pause", NULL, 5000, record, &second, id2, sizeof id2);
    MARY_ASSERT(strcmp(id1, id2) != 0);
    snprintf(line, sizeof line, "{\"type\":\"skill.result\",\"call_id\":\"%s\",\"ok\":false,\"error\":\"denied\"}", id2);
    struct json_object *reply = result_line(line);
    mcu_pipes_on_message(p, reply);
    json_object_put(reply);
    MARY_ASSERT_EQ(second.calls, 1);
    MARY_ASSERT(!second.ok);
    MARY_ASSERT_STR(second.error, "denied");
    MARY_ASSERT_EQ(first.calls, 0);
    snprintf(line, sizeof line, "{\"type\":\"skill.result\",\"call_id\":\"%s\",\"ok\":false}", id1);
    reply = result_line(line);
    mcu_pipes_on_message(p, reply);
    json_object_put(reply);
    MARY_ASSERT_STR(first.error, "failed");   /* no reason given */
    mcu_pipes_free(p);
}

MARY_TEST(timeouts_and_a_lost_connection_end_pending_calls) {
    struct wire w = { 0 };
    mcu_pipes *p = mcu_pipes_new(send_to, &w);
    struct outcome quick = { 0 }, slow = { 0 }, later = { 0 };
    mcu_invoke(p, "a", "b", NULL, 50, record, &quick, NULL, 0);
    mcu_invoke(p, "c", "d", NULL, 60000, record, &slow, NULL, 0);
    mcu_pipes_tick(p, mc_now_ms() + 100);
    MARY_ASSERT_EQ(quick.calls, 1);
    MARY_ASSERT_STR(quick.error, "timeout");
    MARY_ASSERT_EQ(slow.calls, 0);
    mcu_pipes_disconnect(p);
    MARY_ASSERT_STR(slow.error, "disconnected");
    MARY_ASSERT_EQ(mcu_pipes_pending(p), 0);
    mcu_invoke(p, "e", "f", NULL, 60000, record, &later, NULL, 0);
    mcu_pipes_free(p);
    MARY_ASSERT_STR(later.error, "disconnected");   /* freeing ends what was left */
}

MARY_TEST(an_unsendable_call_is_not_left_waiting) {
    struct wire w = { .fail = 1 };
    mcu_pipes *p = mcu_pipes_new(send_to, &w);
    struct outcome o = { 0 };
    MARY_ASSERT_EQ(mcu_invoke(p, "a", "b", NULL, 1000, record, &o, NULL, 0), -EPIPE);
    MARY_ASSERT_EQ(mcu_pipes_pending(p), 0);
    MARY_ASSERT_EQ(o.calls, 0);
    MARY_ASSERT_EQ(mcu_invoke(p, NULL, "b", NULL, 1000, record, &o, NULL, 0), -EINVAL);
    MARY_ASSERT_EQ(mcu_app_state(p, "calendar", record, &o), -ENOSYS);
    mcu_pipes_free(p);
}

int main(void) {
    MARY_RUN(an_invoke_goes_out_and_its_result_comes_back);
    MARY_RUN(results_match_their_calls_in_any_order);
    MARY_RUN(timeouts_and_a_lost_connection_end_pending_calls);
    MARY_RUN(an_unsendable_call_is_not_left_waiting);
    MARY_TEST_MAIN_END();
}
