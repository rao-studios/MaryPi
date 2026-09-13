#include <stdlib.h>
#include <string.h>

#include "common/json.h"
#include "mary_test.h"
#include "sewn/memory.h"

MARY_TEST(the_count_trigger_fires_every_seventh_user_message) {
    MARY_ASSERT(!sewn_memory_count_due(0));
    MARY_ASSERT(!sewn_memory_count_due(6));
    MARY_ASSERT(sewn_memory_count_due(7));
    MARY_ASSERT(sewn_memory_count_due(14));
    MARY_ASSERT(!sewn_memory_count_due(15));
}

MARY_TEST(the_transcript_and_the_sanitised_lines_follow_sewn) {
    const char *history = "[{\"role\":\"user\",\"content\":\"Hi\"},{\"role\":\"assistant\",\"content\":\"\"},{\"role\":\"assistant\",\"content\":\"Hello\"}]";
    struct json_object *messages = mc_json_parse(history, strlen(history));
    char *t = sewn_memory_transcript(messages, "What is Paris?");
    MARY_ASSERT_STR(t, "[user]: Hi\n[assistant]: Hello\n[user]: What is Paris?");
    free(t);
    json_object_put(messages);
    char **lines = NULL;
    size_t n = sewn_memory_lines("**Paris trip**\n\nThe user planned a spring trip to Paris with two friends.\nShort line.\n\n  \nThey chose the Marais for the hotel and April for the dates.\n", &lines);
    MARY_ASSERT_EQ(n, 2);                                 /* the title and "Short line." have four words or fewer */
    MARY_ASSERT_STR(lines[0], "The user planned a spring trip to Paris with two friends.");
    MARY_ASSERT_STR(lines[1], "They chose the Marais for the hotel and April for the dates.");
    sewn_memory_lines_free(lines, n);
    MARY_ASSERT(strstr(sewn_memory_prompt(), "7 words max") != NULL);
}

int main(void) {
    MARY_RUN(the_count_trigger_fires_every_seventh_user_message);
    MARY_RUN(the_transcript_and_the_sanitised_lines_follow_sewn);
    MARY_TEST_MAIN_END();
}
