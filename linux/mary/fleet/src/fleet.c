#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "fleet/gate.h"
#include "fleet/lora.h"

bool fleet_lora_is_slotted(const fleet_lora_entry *entry) {
    return entry && entry->thread_id && *entry->thread_id && entry->ability_id && *entry->ability_id;
}

fleet_gate *fleet_gate_new(const char *schema_template_json, const fleet_vocabulary *vocabulary) {
    errno = ENOSYS;
    return NULL;
}

fleet_gate_state fleet_gate_initial_state(const fleet_gate *gate) {
    fleet_gate_state state;
    memset(&state, 0, sizeof state);
    return state;
}

int fleet_gate_decide(const fleet_gate *gate, const fleet_gate_state *state, fleet_gate_decision *out) {
    return -ENOSYS;
}

void fleet_gate_free(fleet_gate *gate) { free(gate); }

const char *fleet_gate_decision_name(fleet_gate_decision_kind kind) {
    static const char *const names[] = { "forced", "free", "complete", "stuck" };
    return (unsigned)kind < sizeof names / sizeof names[0] ? names[kind] : NULL;
}
