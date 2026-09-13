#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "common/json.h"
#include "mary_test.h"
#include "sewn/complete.h"
#include "sewn/embed.h"

static struct json_object *parse(const char *text) { return mc_json_parse(text, strlen(text)); }

MARY_TEST(the_skills_body_keeps_roles_drops_empties_and_offers_tools) {
    struct json_object *messages = parse("[{\"role\":\"user\",\"content\":\"  \"},{\"role\":\"assistant\",\"content\":\"ok\"},{\"role\":\"user\",\"content\":\"bring Finder forward\"}]");
    struct json_object *tools = parse("[{\"type\":\"function\",\"function\":{\"name\":\"bring_window_forward\",\"parameters\":{\"type\":\"object\"}}}]");
    sewn_complete_request req = { .provider = SEWN_PROVIDER_MISTRAL, .system = "Call tools by name.", .messages = messages, .tools = tools,
                                  .max_tokens = 800, .temperature = 0 };
    struct json_object *body = sewn_complete_body(&req);
    MARY_ASSERT_STR(mc_json_string(body, "model"), "mistral-medium-latest");
    struct json_object *m = mc_json_array(body, "messages");
    MARY_ASSERT_EQ(json_object_array_length(m), 3);          /* system, assistant, user */
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(m, 0), "role"), "system");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(m, 2), "content"), "bring Finder forward");
    double top_p = 0;
    MARY_ASSERT(mc_json_double(body, "top_p", &top_p) && top_p == 1.0);   /* greedy needs top_p 1 */
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(body, "tools")), 1);
    MARY_ASSERT(mc_json_string(body, "stream") == NULL);
    json_object_put(body);
    req.tools = NULL;
    req.model = SEWN_UTILITY_MODEL;
    req.temperature = 0.8;
    body = sewn_complete_body(&req);
    MARY_ASSERT_STR(mc_json_string(body, "model"), "mistral-tiny");
    MARY_ASSERT(mc_json_array(body, "tools") == NULL);
    MARY_ASSERT(mc_json_double(body, "top_p", &top_p) && top_p == 0.9);
    json_object_put(body);
    json_object_put(messages);
    json_object_put(tools);
}

MARY_TEST(mistrals_answer_becomes_text_and_tool_calls) {
    const char *answer = "{\"id\":\"x\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"\","
                         "\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"look\",\"arguments\":\"{\\\"direction\\\":\\\"ahead\\\"}\"}},"
                         "{\"function\":{\"name\":\"open\",\"arguments\":{\"path\":\"/tmp\"}}},{\"function\":{\"name\":\"\"}}]},\"finish_reason\":\"tool_calls\"}],"
                         "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15}}";
    sewn_completion out;
    MARY_ASSERT_EQ(sewn_completion_parse(answer, strlen(answer), &out), 0);
    MARY_ASSERT_STR(out.text, "");
    MARY_ASSERT_EQ(json_object_array_length(out.tool_calls), 2);        /* the nameless one is dropped */
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(out.tool_calls, 0), "name"), "look");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(out.tool_calls, 0), "arguments"), "{\"direction\":\"ahead\"}");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(out.tool_calls, 1), "arguments"), "{\"path\":\"/tmp\"}");   /* an object is serialized */
    MARY_ASSERT_EQ(out.prompt_tokens, 10);
    sewn_completion_free(&out);
    const char *parts = "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Hello \"},{\"type\":\"text\",\"text\":\"there\"}]}}]}";
    MARY_ASSERT_EQ(sewn_completion_parse(parts, strlen(parts), &out), 0);
    MARY_ASSERT_STR(out.text, "Hello there");
    MARY_ASSERT(out.tool_calls == NULL);
    sewn_completion_free(&out);
    MARY_ASSERT_EQ(sewn_completion_parse("{\"error\":\"nope\"}", 14, &out), -EBADMSG);
}

MARY_TEST(prose_tool_calls_are_recovered_and_stripped) {
    char *text = strdup("Sure.\n<tool_call>{\"name\": \"look\", \"arguments\": {\"direction\": \"ahead\"}}</tool_call>\n<tool_call>{\"name\":\"stop\"}</tool_call>  ");
    struct json_object *calls = NULL;
    MARY_ASSERT_EQ(sewn_completion_recover_tool_calls(&text, &calls), 2);
    MARY_ASSERT_STR(text, "Sure.");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(calls, 0), "name"), "look");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(calls, 0), "arguments"), "{\"direction\":\"ahead\"}");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(calls, 1), "arguments"), "{}");
    json_object_put(calls);
    free(text);
    calls = NULL;
    text = strdup("{\"name\": \"look\", \"arguments\": \"{}\"}");
    MARY_ASSERT_EQ(sewn_completion_recover_tool_calls(&text, &calls), 1);   /* a bare object */
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(calls, 0), "name"), "look");
    json_object_put(calls);
    free(text);
    calls = NULL;
    text = strdup("Just prose with {braces} and no name.");
    MARY_ASSERT_EQ(sewn_completion_recover_tool_calls(&text, &calls), 0);
    MARY_ASSERT(calls == NULL);
    free(text);
}

MARY_TEST(embeddings_are_asked_for_as_mistral_wants_and_read_in_index_order) {
    const char *texts[] = { "a", "b" };
    struct json_object *body = sewn_embed_body(texts, 2);
    MARY_ASSERT_STR(mc_json_string(body, "model"), "mistral-embed");
    MARY_ASSERT_STR(mc_json_string(body, "encoding_format"), "float");
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(body, "input")), 2);
    json_object_put(body);
    const char *answer = "{\"data\":[{\"index\":1,\"embedding\":[3,4]},{\"index\":0,\"embedding\":[1,2]}],\"usage\":{}}";
    float out[4];
    size_t dim = 0;
    MARY_ASSERT_EQ(sewn_embed_parse(answer, strlen(answer), 2, out, &dim), 0);
    MARY_ASSERT_EQ(dim, 2);
    MARY_ASSERT_NEAR(out[0], 1, 1e-6);
    MARY_ASSERT_NEAR(out[3], 4, 1e-6);
    MARY_ASSERT_EQ(sewn_embed_parse(answer, strlen(answer), 3, out, &dim), -EBADMSG);
    float a[2] = { 1, 0 }, b[2] = { 0, 1 }, c[2] = { 2, 0 };
    MARY_ASSERT_NEAR(sewn_cosine(a, b, 2), 0, 1e-6);
    MARY_ASSERT_NEAR(sewn_cosine(a, c, 2), 1, 1e-6);
}

int main(void) {
    MARY_RUN(the_skills_body_keeps_roles_drops_empties_and_offers_tools);
    MARY_RUN(mistrals_answer_becomes_text_and_tool_calls);
    MARY_RUN(prose_tool_calls_are_recovered_and_stripped);
    MARY_RUN(embeddings_are_asked_for_as_mistral_wants_and_read_in_index_order);
    MARY_TEST_MAIN_END();
}
