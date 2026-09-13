#include <errno.h>
#include <math.h>

#include "fleet/gate.h"
#include "fleet/lora.h"
#include "mary_test.h"

MARY_TEST(a_slot_names_both_thread_and_ability) {
    fleet_lora_entry smoke = { .model_id = "mlx-community/Qwen3-0.6B-4bit", .eval_exact_match = NAN };
    MARY_ASSERT(!fleet_lora_is_slotted(&smoke));
    smoke.thread_id = "3f1c";
    MARY_ASSERT(!fleet_lora_is_slotted(&smoke));
    smoke.ability_id = "writing.summarize";
    MARY_ASSERT(fleet_lora_is_slotted(&smoke));
    MARY_ASSERT(isnan(smoke.eval_exact_match));
}

MARY_TEST(the_gate_is_declared_but_not_ported) {
    MARY_ASSERT_STR(fleet_gate_decision_name(FLEET_GATE_FORCED), "forced");
    MARY_ASSERT_STR(fleet_gate_decision_name(FLEET_GATE_STUCK), "stuck");
    errno = 0;
    MARY_ASSERT(fleet_gate_new("{}", &(fleet_vocabulary){ 0 }) == NULL);
    MARY_ASSERT_EQ(errno, ENOSYS);
    fleet_gate_decision d;
    MARY_ASSERT_EQ(fleet_gate_decide(NULL, NULL, &d), -ENOSYS);
}

int main(void) {
    MARY_RUN(a_slot_names_both_thread_and_ability);
    MARY_RUN(the_gate_is_declared_but_not_ported);
    MARY_TEST_MAIN_END();
}
