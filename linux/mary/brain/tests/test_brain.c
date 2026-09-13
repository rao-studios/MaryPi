#include <stdlib.h>

#include "brain/clock.h"
#include "brain/history.h"
#include "brain/prompt.h"
#include "brain/request.h"
#include "brain/tools.h"
#include "common/json.h"
#include "mary_test.h"

static const mb_clock SATURDAY_EVENING = { 2026, 9, 12, 6, 19, 5, "America/Los_Angeles" };

MARY_TEST(the_clock_reads_as_dateformatter_writes_it) {
    char text[64];
    mb_clock_time(&SATURDAY_EVENING, text, sizeof text);
    MARY_ASSERT_STR(text, "7:05 PM");
    mb_clock_date(&SATURDAY_EVENING, text, sizeof text);
    MARY_ASSERT_STR(text, "Saturday, September 12, 2026");
    mb_clock midnight = { 2027, 1, 1, 5, 0, 30, "UTC" }, noon = { 2027, 1, 1, 5, 12, 0, "UTC" };
    mb_clock_time(&midnight, text, sizeof text);
    MARY_ASSERT_STR(text, "12:30 AM");
    mb_clock_time(&noon, text, sizeof text);
    MARY_ASSERT_STR(text, "12:00 PM");
    mb_clock now;
    MARY_ASSERT_EQ(mb_clock_now(&now), 0);
    MARY_ASSERT(now.year >= 2026 && now.month >= 1 && now.month <= 12 && now.zone[0]);
}

MARY_TEST(the_voice_instructions_are_the_voice_plan_for_a_maryos_turn) {
    char *text = mb_sewn_instructions(&SATURDAY_EVENING);
    MARY_ASSERT(strncmp(text,
        "Right now it is 7:05 PM on Saturday, September 12, 2026 (America/Los_Angeles); never guess the date or time. "
        "Your words are read aloud by a text-to-speech voice:", 150) == 0);
    const char *company = strstr(text, " Match the user's register.");
    const char *persona = strstr(text, " This turn is CONVERSATION, not a request:");
    const char *retrieval = strstr(text, "\n\nAnything you REMEMBER about their documents");
    MARY_ASSERT(company && persona && retrieval && company < persona && persona < retrieval);
    MARY_ASSERT(strstr(text, "good company first. Match") != NULL);          /* the preamble ends with no space */
    MARY_ASSERT(strstr(text, "do not fill the gap from memory.") != NULL);
    MARY_ASSERT(text[strlen(text) - 1] == '.');                             /* nothing after the retrieval doctrine */
    MARY_ASSERT(strstr(text, "this Mac") == NULL);                          /* no reach or sight on MaryOS yet */
    MARY_ASSERT(strstr(text, "eyes on request") == NULL);
    MARY_ASSERT(strstr(text, "Your hands are running in parallel") == NULL);
    free(text);
}

MARY_TEST(history_keeps_twelve_spoken_messages_by_whole_exchanges) {
    mb_history h;
    mb_history_init(&h);
    for (int i = 0; i < 7; i++) {
        char u[16], a[16];
        snprintf(u, sizeof u, "q%d", i);
        snprintf(a, sizeof a, "a%d", i);
        mb_history_append(&h, MB_ROLE_USER, u);
        mb_history_append(&h, MB_ROLE_SKILL_RESULT, "{\"ok\":true}");
        mb_history_append(&h, MB_ROLE_ASSISTANT, a);
    }
    MARY_ASSERT_EQ(mb_history_spoken_count(&h), 12);
    MARY_ASSERT_STR(h.turns[0].text, "q1");                                 /* the first exchange went whole */
    /* q7 makes thirteen: trimming drops the whole q1 exchange, which lands under the limit,
     * as trimHistory does. An empty reply is not spoken, so it changes nothing. */
    mb_history_append(&h, MB_ROLE_USER, "q7");
    mb_history_append(&h, MB_ROLE_ASSISTANT, "");
    MARY_ASSERT_EQ(mb_history_spoken_count(&h), 11);
    struct json_object *messages = mb_history_spoken_messages(&h);
    MARY_ASSERT_EQ(json_object_array_length(messages), 11);
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(messages, 0), "content"), "q2");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(messages, 10), "content"), "q7");
    json_object_put(messages);
    mb_history_free(&h);

    mb_history giant;
    mb_history_init(&giant);
    mb_history_append(&giant, MB_ROLE_USER, "one question");
    for (int i = 0; i < 20; i++) mb_history_append(&giant, MB_ROLE_ASSISTANT, "and another thought");
    MARY_ASSERT_EQ(mb_history_spoken_count(&giant), 21);                    /* one giant exchange is kept */
    mb_history_free(&giant);
}

MARY_TEST(turn_start_is_marys_chat_request) {
    mb_history h;
    mb_history_init(&h);
    mb_history_append(&h, MB_ROLE_USER, "What is the capital of France?");
    char *instructions = mb_sewn_instructions(&SATURDAY_EVENING);
    mb_turn_request r = { .instructions = instructions, .owner_id = "mary", .request_id = "req-1" };
    struct json_object *start = mb_turn_start(&h, &r);
    MARY_ASSERT_STR(mc_json_type(start), "turn.start");
    struct json_object *request = mc_json_object(start, "request");
    int64_t max_tokens = 0;
    double temperature = 0, top_p = 0;
    MARY_ASSERT(mc_json_int64(request, "max_tokens", &max_tokens) && max_tokens == 1200);
    MARY_ASSERT(mc_json_double(request, "temperature", &temperature));
    MARY_ASSERT_NEAR(temperature, 0.4, 1e-9);
    MARY_ASSERT(mc_json_double(request, "top_p", &top_p));
    MARY_ASSERT_NEAR(top_p, 0.9, 1e-9);
    MARY_ASSERT_STR(mc_json_string(request, "client"), "mary");
    MARY_ASSERT_STR(mc_json_string(request, "provider"), "mistral");
    MARY_ASSERT_STR(mc_json_string(request, "instructions"), instructions);
    MARY_ASSERT_STR(mc_json_string(mc_json_object(request, "persona"), "name"), "Mary");
    MARY_ASSERT(strstr(mc_json_string(mc_json_object(request, "persona"), "voice"), "living in MaryOS") != NULL);
    MARY_ASSERT_STR(mc_json_string(mc_json_object(request, "sewn"), "scope"), "personal");
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(request, "messages")), 1);
    MARY_ASSERT_STR(mc_json_string(mc_json_object(start, "tts"), "voice_id"), "fr_marie_neutral");
    size_t len = 0;
    MARY_ASSERT(strstr(mc_json_compact(request, &len), "\"temperature\":0.4,") != NULL);
    json_object_put(start);
    free(instructions);
    mb_history_free(&h);
    MARY_ASSERT(mb_tools(NULL) == NULL);
}

int main(void) {
    MARY_RUN(the_clock_reads_as_dateformatter_writes_it);
    MARY_RUN(the_voice_instructions_are_the_voice_plan_for_a_maryos_turn);
    MARY_RUN(history_keeps_twelve_spoken_messages_by_whole_exchanges);
    MARY_RUN(turn_start_is_marys_chat_request);
    MARY_TEST_MAIN_END();
}
