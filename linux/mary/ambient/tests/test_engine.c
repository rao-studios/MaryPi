/* AmbientEngine: the classify ladder, the intent gate's memory plan, the lead, and the route. */
#include "ambient/engine.h"
#include "ambient/wire.h"
#include "mary_test.h"
#include <json-c/json.h>

static ma_roster roster;
static ma_route route;

static void resolve(const char *utterance, const char *lead, const ma_world *world) {
    ma_focus_signal focus = { .has_lead = lead != NULL };
    if (lead) focus.lead = ma_place_application(lead);
    ma_engine_inputs in = { .utterance = utterance, .classify_edit = true, .bare_decision = -2, .world = world, .lead_application_id = lead,
                            .focus = &focus, .roster = &roster, .now = 1000 };
    ma_engine_resolve(&in, &route);
}

MARY_TEST(a_chat_is_converse_and_needs_nothing) {
    resolve("how was your day?", NULL, NULL);
    MARY_ASSERT_EQ(route.intent, MA_INTENT_CONVERSE);
    MARY_ASSERT_EQ(route.decided_by, MA_SIGNAL_NONE);
    MARY_ASSERT(!route.needs_execution && !route.needs_locate && !route.needs_pre_read);
    MARY_ASSERT_EQ(route.gate.memory.lanes, MA_LANE_PERSONAL);
    MARY_ASSERT(route.lead_application_id[0] == 0);
    MARY_ASSERT_EQ(route.ranking_mode, MA_RANKING_RELEVANCE);
    MARY_ASSERT_EQ(route.candidate_attentions, 31u);       /* every lane, none with eyes */
    MARY_ASSERT(!ma_route_is_action_turn(&route));
}

MARY_TEST(the_ladder_in_order) {
    resolve("tighten the intro", "textedit", NULL);
    MARY_ASSERT_EQ(route.intent, MA_INTENT_REVISE);
    MARY_ASSERT_EQ(route.decided_by, MA_SIGNAL_EDIT_INTENT);
    MARY_ASSERT_EQ(route.writing_target, MA_WRITING_PASSAGE);
    MARY_ASSERT(route.needs_locate && route.needs_execution && ma_route_is_action_turn(&route));
    MARY_ASSERT_STR(route.verdicts.edit.target[0], "intro");
    MARY_ASSERT(route.verdicts.names_transform);
    MARY_ASSERT_STR(route.verdicts.focus_override, "writing");

    resolve("teach you a new ability", NULL, NULL);
    MARY_ASSERT_EQ(route.intent, MA_INTENT_ARCHITECT);
    MARY_ASSERT_EQ(route.decided_by, MA_SIGNAL_ARCHITECT_ABILITY);
    MARY_ASSERT(!route.needs_execution);

    resolve("what's on my screen", "finder", NULL);
    MARY_ASSERT_EQ(route.intent, MA_INTENT_PERCEIVE);
    MARY_ASSERT_EQ(route.decided_by, MA_SIGNAL_DEIXIS);
    MARY_ASSERT_EQ(route.ranking_mode, MA_RANKING_FOCUSED_WORLD);

    resolve("what is TextEdit showing", "textedit", NULL);
    MARY_ASSERT_EQ(route.intent, MA_INTENT_PERCEIVE);
    MARY_ASSERT_EQ(route.decided_by, MA_SIGNAL_NAMED_LEAD_ATTENTION);

    resolve("what's on my calendar tomorrow", NULL, NULL);
    MARY_ASSERT_EQ(route.intent, MA_INTENT_ASK);
    MARY_ASSERT_EQ(route.decided_by, MA_SIGNAL_AMBIENT_SOURCE);

    resolve("read me the section about batteries", "textedit", NULL);
    MARY_ASSERT_EQ(route.intent, MA_INTENT_ASK);
    MARY_ASSERT_EQ(route.decided_by, MA_SIGNAL_NAMED_PART);
    MARY_ASSERT(route.needs_pre_read);
    MARY_ASSERT_STR(route.verdicts.named_part, "batteries");
}

MARY_TEST(decisions_and_stops_come_first) {
    ma_engine_inputs in = { .utterance = "yes", .classify_edit = true, .bare_decision = -2, .pending_confirmation = true, .roster = &roster, .now = 1000 };
    ma_engine_resolve(&in, &route);
    MARY_ASSERT_EQ(route.intent, MA_INTENT_DECIDE);
    MARY_ASSERT_EQ(route.decided_by, MA_SIGNAL_PENDING_DECISION);
    MARY_ASSERT_EQ(route.verdicts.bare_decision, 1);
    in.pending_confirmation = false;
    in.active_routines = 1;
    in.utterance = "no";
    ma_engine_resolve(&in, &route);
    MARY_ASSERT_EQ(route.intent, MA_INTENT_HALT);
    MARY_ASSERT_EQ(route.decided_by, MA_SIGNAL_ROUTINE_STOP);
    in.utterance = "play the next song";
    in.action_turn = true;
    in.lead_application_id = "media";
    ma_engine_resolve(&in, &route);
    MARY_ASSERT_EQ(route.intent, MA_INTENT_OPERATE);
    MARY_ASSERT_EQ(route.decided_by, MA_SIGNAL_ACTION_COMMAND);
    in.utterance = "write a poem";
    in.lead_application_id = "textedit";
    ma_engine_resolve(&in, &route);
    MARY_ASSERT_EQ(route.intent, MA_INTENT_COMPOSE);
    MARY_ASSERT_EQ(route.decided_by, MA_SIGNAL_WRITING_REGISTER);
    in.action_turn = false;
    in.has_embedding_intent = true;
    in.embedding_intent = MA_INTENT_OPERATE;
    ma_engine_resolve(&in, &route);
    MARY_ASSERT_EQ(route.decided_by, MA_SIGNAL_EMBEDDING);
}

MARY_TEST(a_literally_named_place_outranks_the_frontmost_one) {
    resolve("play the next song in the Media Player", "textedit", NULL);
    MARY_ASSERT_STR(route.lead_application_id, "media");
    MARY_ASSERT_EQ(route.named_count, 1);
    MARY_ASSERT_STR(route.named_places[0].application, "media");
    MARY_ASSERT_EQ(route.gate.application_count, 1);
    MARY_ASSERT_STR(route.gate.applications[0], "media");
    MARY_ASSERT(route.realm.has_place);
    MARY_ASSERT_STR(route.realm.place.application, "media");
    /* the memory plan prefers the ability thread when an app is named */
    MARY_ASSERT_EQ(route.gate.memory.lanes, MA_LANE_ABILITY);
    MARY_ASSERT_EQ(route.gate.memory.target_count, 1);
    MARY_ASSERT_STR(route.gate.memory.targets[0].ability_id, "multimedia");
    MARY_ASSERT(route.gate.memory.expand_discipline_usage);
    MARY_ASSERT(route.gate.memory.hint_count >= 2);      /* "multimedia", "practices" */
    MARY_ASSERT_STR(route.gate.memory.relationship_hints[0], "multimedia");
    MARY_ASSERT_STR(route.gate.memory.relationship_hints[1], "practices");
    const char *lanes[2];
    MARY_ASSERT_EQ(ma_lane_storage_lanes(MA_LANE_ABILITY, lanes, 2), 2);
    MARY_ASSERT_STR(lanes[0], "application");
    MARY_ASSERT_STR(lanes[1], "behavioral");
}

MARY_TEST(question_forms_shape_the_plan) {
    resolve("how do I save in TextEdit?", "textedit", NULL);
    MARY_ASSERT_EQ(route.gate.questions, MA_Q_HOW);
    MARY_ASSERT_EQ(route.gate.memory.lanes, MA_LANE_ABILITY);
    MARY_ASSERT_EQ(route.gate.memory.lane_priority[0], MA_LANE_ABILITY);
    MARY_ASSERT_STR(route.gate.signature.predicate_families[0], "operates");
    MARY_ASSERT(strstr(route.gate.signature.semantic_projection, "[relationships: operates, supports, workflow]") != NULL);
    resolve("why did we write the essay in TextEdit", "textedit", NULL);
    MARY_ASSERT_EQ(route.gate.memory.lanes, MA_LANE_ABILITY | MA_LANE_PERSONAL);
    MARY_ASSERT_EQ(route.gate.memory.lane_priority[0], MA_LANE_PERSONAL);
    resolve("who wrote this", NULL, NULL);
    MARY_ASSERT_EQ(route.gate.memory.lanes, MA_LANE_PERSONAL);
    MARY_ASSERT_EQ(route.gate.memory.priority_count, 1);
}

MARY_TEST(a_fresh_selection_defines_a_deictic_turn_unless_another_app_is_named) {
    ma_world world = { .sense = MA_SENSE_SELECTION, .attention = MA_ATTENTION_APPLICATIONS, .captured_at = 995, .fresh_for = 30, .editable = true };
    snprintf(world.application_id, sizeof world.application_id, "textedit");
    snprintf(world.selected_text, sizeof world.selected_text, "the tide comes in");
    resolve("tighten this", NULL, &world);
    MARY_ASSERT(route.selection_defines_turn);
    MARY_ASSERT_EQ(route.intent, MA_INTENT_REVISE);
    MARY_ASSERT_EQ(route.writing_target, MA_WRITING_SELECTION);
    MARY_ASSERT_STR(route.lead_application_id, "textedit");
    MARY_ASSERT(ma_route_routed_world(&route) != NULL);
    resolve("what does this say", NULL, &world);
    MARY_ASSERT_EQ(route.intent, MA_INTENT_PERCEIVE);
    MARY_ASSERT_EQ(route.decided_by, MA_SIGNAL_WORLD);
    resolve("tighten this in the Finder", NULL, &world);
    MARY_ASSERT(!route.selection_defines_turn);
    MARY_ASSERT(ma_route_routed_world(&route) == NULL && route.has_world);
    MARY_ASSERT_STR(route.lead_application_id, "finder");
    world.captured_at = 900;                                    /* stale: no attention at all */
    resolve("tighten this", "textedit", &world);
    MARY_ASSERT(!route.has_world && !route.selection_defines_turn);
    MARY_ASSERT_EQ(route.writing_target, MA_WRITING_PASSAGE);
}

MARY_TEST(the_route_serialises_with_the_report_vocabulary) {
    resolve("read me the section about batteries", "textedit", NULL);
    struct json_object *o = ma_route_json(&route, &roster);
    const char *text = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN);
    MARY_ASSERT(strstr(text, "\"intent\":\"ask\"") && strstr(text, "\"decidedBy\":\"namedPart\""));
    MARY_ASSERT(strstr(text, "\"leadApplicationID\":\"textedit\"") && strstr(text, "\"token\":\"applications:textedit\""));
    MARY_ASSERT(strstr(text, "\"storageLanes\":[\"personal\",\"conversation\"]") != NULL);
    MARY_ASSERT(strstr(text, "\"namedPart\":\"batteries\"") != NULL);
    json_object_put(o);
}

int main(void) {
    ma_roster_maryos(&roster);
    MARY_RUN(a_chat_is_converse_and_needs_nothing);
    MARY_RUN(the_ladder_in_order);
    MARY_RUN(decisions_and_stops_come_first);
    MARY_RUN(a_literally_named_place_outranks_the_frontmost_one);
    MARY_RUN(question_forms_shape_the_plan);
    MARY_RUN(a_fresh_selection_defines_a_deictic_turn_unless_another_app_is_named);
    MARY_RUN(the_route_serialises_with_the_report_vocabulary);
    MARY_TEST_MAIN_END();
}
