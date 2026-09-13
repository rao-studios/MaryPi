#include "ambient/intent.h"
#include "mary_test.h"

MARY_TEST(names_are_the_swift_raw_values) {
    MARY_ASSERT_STR(ma_intent_name(MA_INTENT_CONVERSE), "converse");
    MARY_ASSERT_STR(ma_intent_name(MA_INTENT_ARCHITECT), "architect");
    MARY_ASSERT(ma_intent_name(MA_INTENT_COUNT) == NULL);
    MARY_ASSERT_STR(ma_signal_name(MA_SIGNAL_WORLD), "attention");
    MARY_ASSERT_STR(ma_signal_name(MA_SIGNAL_NAMED_LEAD_ATTENTION), "namedLeadWorld");
    ma_intent parsed = MA_INTENT_ASK;
    MARY_ASSERT(ma_intent_from_name("operate", &parsed));
    MARY_ASSERT_EQ(parsed, MA_INTENT_OPERATE);
    MARY_ASSERT(!ma_intent_from_name("Operate", &parsed));
}

MARY_TEST(acting_shapes_match_is_acting) {
    int acting = 0;
    for (int i = 0; i < MA_INTENT_COUNT; i++) acting += ma_intent_is_acting((ma_intent)i);
    MARY_ASSERT_EQ(acting, 5);
    MARY_ASSERT(!ma_intent_is_acting(MA_INTENT_CONVERSE));
    MARY_ASSERT(ma_intent_is_acting(MA_INTENT_HALT));
    MARY_ASSERT(ma_intent_touches_existing_prose(MA_INTENT_REVISE));
    MARY_ASSERT(!ma_intent_touches_existing_prose(MA_INTENT_COMPOSE));
}

int main(void) {
    MARY_RUN(names_are_the_swift_raw_values);
    MARY_RUN(acting_shapes_match_is_acting);
    MARY_TEST_MAIN_END();
}
