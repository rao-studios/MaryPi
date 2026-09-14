/* Lane B over scripted hooks: tool calls dispatched and paired, the repeat guard, a confirmation that
 * parks and resumes, a denied skill, the continuation nudge once, and the ten-round cap. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "brain/lane.h"
#include "brain/prompt.h"
#include "common/json.h"
#include "mary_test.h"

static sk_registry registry;
static const char REGISTRY[] =
    "{\"type\":\"skills\",\"apps\":["
    "{\"id\":\"media\",\"name\":\"Media Player\",\"enabled\":true,\"ask\":\"never\",\"summary\":\"Plays music.\",\"skills\":["
    "{\"id\":\"play_pause\",\"title\":\"Play or pause\",\"summary\":\"Plays or pauses.\",\"params\":null,\"effect\":\"act\",\"enabled\":true},"
    "{\"id\":\"now_playing\",\"title\":\"Now playing\",\"summary\":\"What plays.\",\"params\":null,\"effect\":\"read\",\"enabled\":true}]},"
    "{\"id\":\"finder\",\"name\":\"Finder\",\"enabled\":true,\"ask\":\"always\",\"summary\":\"Files.\",\"skills\":["
    "{\"id\":\"trash\",\"title\":\"Move to Trash\",\"summary\":\"Trashes a file.\",\"params\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]},\"effect\":\"destructive\",\"enabled\":true}]},"
    "{\"id\":\"settings\",\"name\":\"System Settings\",\"enabled\":false,\"ask\":\"never\",\"skills\":[{\"id\":\"open_pane\",\"title\":\"Open a pane\",\"summary\":\"\",\"params\":null,\"effect\":\"read\",\"enabled\":true}]}]}";

/* The scripted model: one reply per round, and what it saw. */
static const char *script[8];
static int script_len, round_seen, last_messages;
static char last_system[16384], last_tool_result[600], invoked[8][128];
static int invoke_count, confirm_calls, confirm_answer, last_confirmed;
static bool invoke_fails;

static int fake_complete(struct json_object *request, struct json_object **reply, char *message, size_t cap, void *user) {
    struct json_object *messages = mc_json_array(request, "messages");
    last_messages = messages ? (int)json_object_array_length(messages) : 0;
    snprintf(last_system, sizeof last_system, "%s", mc_json_string(request, "instructions") ? mc_json_string(request, "instructions") : "");
    last_tool_result[0] = 0;
    for (int i = last_messages - 1; i >= 0; i--) {
        struct json_object *m = json_object_array_get_idx(messages, i);
        const char *role = mc_json_string(m, "role");
        if (role && strcmp(role, "tool") == 0) { snprintf(last_tool_result, sizeof last_tool_result, "%s", mc_json_string(m, "content")); break; }
    }
    if (round_seen >= script_len) { snprintf(message, cap, "the script ran out"); return -EIO; }
    *reply = mc_json_parse(script[round_seen], strlen(script[round_seen]));
    round_seen++;
    return 0;
}

static int fake_invoke(const char *app, const char *skill, struct json_object *args, bool confirmed, struct json_object **result, char *error, size_t cap, void *user) {
    last_confirmed = confirmed;
    if (invoke_count < 8) snprintf(invoked[invoke_count], 128, "%s.%s %s", app, skill, args ? mc_json_compact(args, NULL) : "");
    invoke_count++;
    if (invoke_fails) { snprintf(error, cap, "failed"); return -EIO; }
    *result = mc_json_parse("{\"ok\":true}", 11);
    return 0;
}

static int fake_confirm(const char *call_id, const sk_app *app, const sk_skill *skill, struct json_object *args, const char *summary, void *user) {
    confirm_calls++;
    return confirm_answer;
}

static const mb_lane_hooks HOOKS = { .complete = fake_complete, .invoke = fake_invoke, .confirm = fake_confirm };

static void reset(void) { round_seen = invoke_count = confirm_calls = last_confirmed = 0; invoke_fails = false; confirm_answer = 1; }

static struct json_object *messages_with(const char *user) {
    struct json_object *a = json_object_new_array(), *m = json_object_new_object();
    json_object_object_add(m, "role", json_object_new_string("user"));
    json_object_object_add(m, "content", json_object_new_string(user));
    json_object_array_add(a, m);
    return a;
}

MARY_TEST(tool_calls_are_dispatched_paired_and_the_prose_kept) {
    reset();
    script[0] = "{\"type\":\"complete.result\",\"text\":\"\",\"tool_calls\":[{\"id\":\"c1\",\"name\":\"media__play_pause\",\"arguments\":\"{}\"}]}";
    script[1] = "{\"type\":\"complete.result\",\"text\":\"Playing.\",\"tool_calls\":[]}";
    script_len = 2;
    struct json_object *messages = messages_with("play the music");
    mb_lane_request req = { .system = "SYSTEM", .messages = messages, .registry = &registry, .provider = "mistral", .request_id = "r1" };
    mb_lane_result out;
    MARY_ASSERT_EQ(mb_lane_run(&req, &HOOKS, &out), 0);
    MARY_ASSERT_EQ(out.rounds, 2);
    MARY_ASSERT_STR(out.text, "Playing.");
    MARY_ASSERT_EQ(out.outcome_count, 1);
    MARY_ASSERT(out.outcomes[0].ok && !out.outcomes[0].landed && !out.outcomes[0].is_read);   /* delivered, unproven: no receipt said it landed */
    MARY_ASSERT_STR(out.outcomes[0].invocation, "media__play_pause");
    MARY_ASSERT_STR(invoked[0], "media.play_pause {}");
    MARY_ASSERT_EQ(last_messages, 3);       /* user, the assistant's tool call, its result */
    MARY_ASSERT_STR(last_tool_result, "RAN, unproven: {\"ok\":true}");
    MARY_ASSERT(strncmp(last_system, "SYSTEM\n\n=== Executor mode ===", 28) == 0);
    mb_lane_result_free(&out);
    json_object_put(messages);
}

MARY_TEST(a_failed_call_is_not_tried_again_with_the_same_words) {
    reset();
    invoke_fails = true;
    script[0] = "{\"type\":\"complete.result\",\"text\":\"\",\"tool_calls\":[{\"id\":\"c1\",\"name\":\"media__play_pause\",\"arguments\":\"{}\"}]}";
    script[1] = "{\"type\":\"complete.result\",\"text\":\"\",\"tool_calls\":[{\"id\":\"c2\",\"name\":\"media__play_pause\",\"arguments\":\"{ }\"}]}";
    script[2] = "{\"type\":\"complete.result\",\"text\":\"It did not work.\",\"tool_calls\":[]}";
    script_len = 3;
    struct json_object *messages = messages_with("play");
    mb_lane_request req = { .system = "S", .messages = messages, .registry = &registry };
    mb_lane_result out;
    MARY_ASSERT_EQ(mb_lane_run(&req, &HOOKS, &out), 0);
    MARY_ASSERT_EQ(invoke_count, 1);        /* the repeat was refused, not dispatched */
    MARY_ASSERT_EQ(out.outcome_count, 1);
    MARY_ASSERT(!out.outcomes[0].ok);
    MARY_ASSERT(strncmp(last_tool_result, "Already tried media__play_pause with the same words this turn", 60) == 0);
    MARY_ASSERT_STR(out.text, "It did not work.");
    mb_lane_result_free(&out);
    json_object_put(messages);
    /* an unproven act that ran is held for a look */
    reset();
    script[0] = "{\"type\":\"complete.result\",\"text\":\"\",\"tool_calls\":[{\"id\":\"c1\",\"name\":\"media__play_pause\",\"arguments\":\"{}\"}]}";
    script[1] = "{\"type\":\"complete.result\",\"text\":\"\",\"tool_calls\":[{\"id\":\"c2\",\"name\":\"media__play_pause\",\"arguments\":\"{}\"}]}";
    script[2] = "{\"type\":\"complete.result\",\"text\":\"Done.\",\"tool_calls\":[]}";
    messages = messages_with("play");
    req.messages = messages;
    MARY_ASSERT_EQ(mb_lane_run(&req, &HOOKS, &out), 0);
    MARY_ASSERT_EQ(invoke_count, 1);
    MARY_ASSERT(strstr(last_tool_result, "already ran with those words and the change is unproven") != NULL);
    mb_lane_result_free(&out);
    json_object_put(messages);
}

MARY_TEST(a_protected_skill_parks_for_the_person_and_a_denied_one_says_so) {
    reset();
    script[0] = "{\"type\":\"complete.result\",\"text\":\"\",\"tool_calls\":[{\"id\":\"c1\",\"name\":\"finder__trash\",\"arguments\":\"{\\\"path\\\":\\\"/home/mary/old.txt\\\"}\"}]}";
    script[1] = "{\"type\":\"complete.result\",\"text\":\"Trashed it.\",\"tool_calls\":[]}";
    script_len = 2;
    struct json_object *messages = messages_with("trash the old file");
    mb_lane_request req = { .system = "S", .messages = messages, .registry = &registry };
    mb_lane_result out;
    MARY_ASSERT_EQ(mb_lane_run(&req, &HOOKS, &out), 0);
    MARY_ASSERT_EQ(confirm_calls, 1);
    MARY_ASSERT_EQ(invoke_count, 1);        /* allowed: it ran */
    MARY_ASSERT_EQ(last_confirmed, 1);      /* marked as the person's answer, so the desktop's gate lets it through */
    MARY_ASSERT(out.outcomes[0].ok);
    MARY_ASSERT_STR(invoked[0], "finder.trash {\"path\":\"/home/mary/old.txt\"}");
    mb_lane_result_free(&out);
    json_object_put(messages);
    /* refused: the lane ends on the question, and nothing ran */
    reset();
    confirm_answer = 0;
    script_len = 2;
    messages = messages_with("trash the old file");
    req.messages = messages;
    MARY_ASSERT_EQ(mb_lane_run(&req, &HOOKS, &out), 0);
    MARY_ASSERT_EQ(invoke_count, 0);
    MARY_ASSERT_EQ(out.rounds, 1);          /* ASKED ends the lane */
    MARY_ASSERT(out.outcomes[0].requested && out.outcomes[0].asks_the_person);
    mb_lane_result_free(&out);
    json_object_put(messages);
    /* a skill the person turned off is not run, and the model is told */
    reset();
    script[0] = "{\"type\":\"complete.result\",\"text\":\"\",\"tool_calls\":[{\"id\":\"c1\",\"name\":\"settings__open_pane\",\"arguments\":\"{}\"},{\"id\":\"c2\",\"name\":\"mail__send\",\"arguments\":\"{}\"}]}";
    script[1] = "{\"type\":\"complete.result\",\"text\":\"I can't.\",\"tool_calls\":[]}";
    script_len = 2;
    messages = messages_with("open sound");
    req.messages = messages;
    MARY_ASSERT_EQ(mb_lane_run(&req, &HOOKS, &out), 0);
    MARY_ASSERT_EQ(invoke_count, 0);
    MARY_ASSERT_EQ(out.outcome_count, 2);
    MARY_ASSERT_STR(out.outcomes[0].summary, "turned off in Settings");
    MARY_ASSERT_STR(out.outcomes[1].summary, "no such skill");
    MARY_ASSERT_EQ(last_messages, 4);       /* user, assistant, two tool results */
    mb_lane_result_free(&out);
    json_object_put(messages);
}

MARY_TEST(the_lane_nudges_once_when_only_reads_ran_on_an_acting_turn_and_stops_at_ten_rounds) {
    reset();
    script[0] = "{\"type\":\"complete.result\",\"text\":\"\",\"tool_calls\":[{\"id\":\"c1\",\"name\":\"media__now_playing\",\"arguments\":\"{}\"}]}";
    script[1] = "{\"type\":\"complete.result\",\"text\":\"Moonlight Sonata is playing.\",\"tool_calls\":[]}";
    script[2] = "{\"type\":\"complete.result\",\"text\":\"\",\"tool_calls\":[{\"id\":\"c2\",\"name\":\"media__play_pause\",\"arguments\":\"{}\"}]}";
    script[3] = "{\"type\":\"complete.result\",\"text\":\"Paused.\",\"tool_calls\":[]}";
    script_len = 4;
    struct json_object *messages = messages_with("pause the music");
    mb_lane_request req = { .system = "S", .messages = messages, .registry = &registry, .implies_action = true };
    mb_lane_result out;
    MARY_ASSERT_EQ(mb_lane_run(&req, &HOOKS, &out), 0);
    MARY_ASSERT(out.nudged);
    MARY_ASSERT_EQ(out.rounds, 4);
    MARY_ASSERT_EQ(out.outcome_count, 2);
    MARY_ASSERT_STR(out.text, "Paused.");
    mb_lane_result_free(&out);
    json_object_put(messages);
    /* the cap: a model that never stops calling is cut off at ten rounds */
    reset();
    for (int i = 0; i < 8; i++) script[i] = "{\"type\":\"complete.result\",\"text\":\"\",\"tool_calls\":[{\"id\":\"c\",\"name\":\"media__now_playing\",\"arguments\":\"{}\"}]}";
    script_len = 8;
    messages = messages_with("what plays");
    req.messages = messages;
    req.implies_action = false;
    req.max_rounds = 3;
    MARY_ASSERT_EQ(mb_lane_run(&req, &HOOKS, &out), 0);
    MARY_ASSERT_EQ(out.rounds, 3);
    MARY_ASSERT_EQ(invoke_count, 3);
    mb_lane_result_free(&out);
    json_object_put(messages);
    /* no completion at all: the lane says why */
    reset();
    script_len = 0;
    messages = messages_with("play");
    req.messages = messages;
    MARY_ASSERT_EQ(mb_lane_run(&req, &HOOKS, &out), -EIO);
    MARY_ASSERT_STR(out.error, "the script ran out");
    mb_lane_result_free(&out);
    json_object_put(messages);
}

MARY_TEST(the_skills_prompt_names_the_roster_and_what_is_in_hand) {
    static const mb_clock CLOCK = { 2026, 9, 12, 6, 19, 5, "America/Los_Angeles" };
    const char *lead[] = { "On screen: TextEdit \xE2\x80\x94 \"Tides\" \xE2\x80\x94 seen 1s ago" }, *held[] = { "TextEdit \xE2\x80\x94 the document, seen 9s ago:\nThe tide." };
    mb_system_inputs in = { .registry = &registry, .lead_place_name = "TextEdit", .lead_context = lead, .lead_context_count = 1, .held_facts = held, .held_fact_count = 1 };
    char *text = mb_system_prompt(&CLOCK, &in);
    MARY_ASSERT(strncmp(text, "You are Mary \xE2\x80\x94 that is your name", 30) == 0);
    MARY_ASSERT(strstr(text, "Right now it is 7:05 PM on Saturday, September 12, 2026 (America/Los_Angeles)") != NULL);
    MARY_ASSERT(strstr(text, "\n- Media Player \xE2\x80\x94 Plays music. (skills: media__play_pause, media__now_playing)") != NULL);
    MARY_ASSERT(strstr(text, "System Settings") == NULL);       /* turned off: not on the roster */
    const char *working = strstr(text, "\n\n=== Working in TextEdit ===\n\nOn screen: TextEdit"), *hand = strstr(text, "\n\n=== Still in hand ===");
    MARY_ASSERT(working && hand && working < hand);
    MARY_ASSERT(strstr(hand, "The tide.") != NULL);
    free(text);
    MARY_ASSERT(strncmp(mb_orchestrator_addendum(), "=== Executor mode ===", 21) == 0);
    MARY_ASSERT(strncmp(mb_continuation_nudge(), "Continuation note:", 18) == 0);
}

int main(void) {
    struct json_object *msg = mc_json_parse(REGISTRY, strlen(REGISTRY));
    sk_registry_init(&registry);
    if (!msg || sk_registry_load(&registry, msg) != 0) { fprintf(stderr, "the fixture registry did not load\n"); return 1; }
    json_object_put(msg);
    MARY_RUN(tool_calls_are_dispatched_paired_and_the_prose_kept);
    MARY_RUN(a_failed_call_is_not_tried_again_with_the_same_words);
    MARY_RUN(a_protected_skill_parks_for_the_person_and_a_denied_one_says_so);
    MARY_RUN(the_lane_nudges_once_when_only_reads_ran_on_an_acting_turn_and_stops_at_ten_rounds);
    MARY_RUN(the_skills_prompt_names_the_roster_and_what_is_in_hand);
    sk_registry_free(&registry);
    MARY_TEST_MAIN_END();
}
