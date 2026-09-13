#include <errno.h>

#include "frigate/frigate.h"
#include "mary_test.h"

MARY_TEST(nothing_is_on_device_yet) {
    MARY_ASSERT(!frigate_available());
    errno = 0;
    MARY_ASSERT(frigate_embedder_new(NULL) == NULL);
    MARY_ASSERT_EQ(errno, ENOSYS);
    errno = 0;
    MARY_ASSERT(frigate_llm_new(FRIGATE_DEFAULT_LLM_MODEL) == NULL);
    MARY_ASSERT_EQ(errno, ENOSYS);
    MARY_ASSERT_EQ(frigate_llm_generate(NULL, "Hello", FRIGATE_DEFAULT_MAX_TOKENS, NULL, NULL), -ENOSYS);
    MARY_ASSERT(frigate_boost_open("model.json") == NULL);
}

MARY_TEST(defaults_match_the_swift_initializers) {
    MARY_ASSERT_STR(FRIGATE_DEFAULT_EMBEDDER_MODEL, "mlx-community/Qwen3-Embedding-0.6B-8bit");
    MARY_ASSERT_STR(FRIGATE_DEFAULT_LLM_MODEL, "mlx-community/Qwen3-0.6B-4bit");
}

int main(void) {
    MARY_RUN(nothing_is_on_device_yet);
    MARY_RUN(defaults_match_the_swift_initializers);
    MARY_TEST_MAIN_END();
}
