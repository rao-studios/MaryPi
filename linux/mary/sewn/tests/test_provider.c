#include <errno.h>
#include <string.h>

#include "mary_test.h"
#include "sewn/mistral.h"
#include "sewn/provider.h"

MARY_TEST(absent_means_mistral_and_only_mistral_is_served) {
    sewn_provider p = SEWN_PROVIDER_TINKER;
    MARY_ASSERT_EQ(sewn_provider_parse(NULL, &p), 0);
    MARY_ASSERT_EQ(p, SEWN_PROVIDER_MISTRAL);
    MARY_ASSERT_EQ(sewn_provider_parse("", &p), 0);
    MARY_ASSERT_EQ(p, SEWN_PROVIDER_MISTRAL);
    MARY_ASSERT_EQ(sewn_provider_parse("Mistral", &p), 0);
    MARY_ASSERT(sewn_provider_available(p));
    MARY_ASSERT_STR(sewn_provider_host(p), "api.mistral.ai");
    MARY_ASSERT_EQ(sewn_provider_parse("tinker", &p), 0);
    MARY_ASSERT_EQ(p, SEWN_PROVIDER_TINKER);
    MARY_ASSERT(!sewn_provider_available(p));         /* a toggle for a later implementation */
    MARY_ASSERT(sewn_provider_host(p) == NULL);
    MARY_ASSERT_STR(sewn_provider_display_name(p), "Thinking Machines (Hosted)");
    MARY_ASSERT_EQ(sewn_provider_parse("local", &p), -EINVAL);
    MARY_ASSERT_EQ(p, SEWN_PROVIDER_TINKER);          /* untouched */
}

MARY_TEST(a_requested_model_is_honoured_only_when_it_is_the_providers) {
    MARY_ASSERT_STR(sewn_provider_chat_model(SEWN_PROVIDER_MISTRAL, "mistral-small-latest"), "mistral-small-latest");
    MARY_ASSERT_STR(sewn_provider_chat_model(SEWN_PROVIDER_MISTRAL, "Codestral-latest"), "Codestral-latest");
    MARY_ASSERT_STR(sewn_provider_chat_model(SEWN_PROVIDER_MISTRAL, "inkling-small"), SEWN_CHAT_MODEL);
    MARY_ASSERT_STR(sewn_provider_chat_model(SEWN_PROVIDER_MISTRAL, NULL), SEWN_CHAT_MODEL);
    MARY_ASSERT(sewn_provider_chat_model(SEWN_PROVIDER_TINKER, "mistral-tiny") == NULL);
    MARY_ASSERT(sewn_is_mistral_model("ministral-8b"));
    MARY_ASSERT(sewn_is_mistral_model("open-mistral-nemo"));
    MARY_ASSERT(!sewn_is_mistral_model("gpt-4"));
}

int main(void) {
    MARY_RUN(absent_means_mistral_and_only_mistral_is_served);
    MARY_RUN(a_requested_model_is_honoured_only_when_it_is_the_providers);
    MARY_TEST_MAIN_END();
}
