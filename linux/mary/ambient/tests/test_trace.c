/* AmbientTraceLog and the wire: a ring of fifty, runs and retrieval attached by turn, the report. */
#include "ambient/trace.h"
#include "ambient/wire.h"
#include "common/buf.h"
#include "mary_test.h"
#include <errno.h>
#include <json-c/json.h>

static ma_roster roster;

static void record(ma_trace_log *log, const char *id, const char *utterance, double at) {
    ma_trace_record *r = calloc(1, sizeof *r);
    snprintf(r->id, sizeof r->id, "%s", id);
    snprintf(r->utterance, sizeof r->utterance, "%s", utterance);
    r->date = at;
    ma_engine_inputs in = { .utterance = utterance, .classify_edit = true, .bare_decision = -2, .roster = &roster, .now = at };
    ma_engine_resolve(&in, &r->route);
    r->system_prompt_chars = 1234;
    ma_trace_push(log, r);
    free(r);
}

MARY_TEST(the_ring_keeps_the_newest_fifty_newest_first) {
    ma_trace_log *log = ma_trace_log_new(0);
    for (int i = 0; i < 55; i++) {
        char id[16];
        snprintf(id, sizeof id, "turn-%d", i);
        record(log, id, "hello", 1000 + i);
    }
    MARY_ASSERT_EQ(ma_trace_count(log), 50);
    MARY_ASSERT_STR(ma_trace_entry(log, 0)->id, "turn-54");
    MARY_ASSERT_STR(ma_trace_entry(log, 49)->id, "turn-5");
    MARY_ASSERT(ma_trace_entry(log, 50) == NULL && ma_trace_find(log, "turn-4") == NULL);
    ma_trace_clear(log);
    MARY_ASSERT_EQ(ma_trace_count(log), 0);
    ma_trace_log_free(log);
}

MARY_TEST(runs_and_retrieval_attach_to_their_turn) {
    ma_trace_log *log = ma_trace_log_new(5);
    record(log, "t1", "play the next song", 1000);
    record(log, "t2", "read me the section about batteries", 1001);
    ma_trace_note_skill_invocation(log, "t1", "media", "next", "act", "{}", 1000.5);
    ma_trace_note_skill_result(log, "t1", "media", "next", "completed", false, "{\"ok\":true}", 1000.9);
    ma_trace_note_skill_result(log, "t1", "media", "play", "completed", false, NULL, 1000.9);       /* no such run: ignored */
    const ma_trace_record *t1 = ma_trace_find(log, "t1");
    MARY_ASSERT_EQ(t1->run_count, 1);
    MARY_ASSERT_STR(t1->runs[0].status, "completed");
    MARY_ASSERT_STR(t1->runs[0].invocation, "media.next");
    MARY_ASSERT_NEAR(t1->runs[0].finished, 1000.9, 1e-9);
    ma_retrieval_purpose purpose = { .name = "context", .lane_count = 2, .returned_count = 1 };
    snprintf(purpose.lanes[0], 16, "personal");
    snprintf(purpose.lanes[1], 16, "behavioral");
    snprintf(purpose.returned[0].document_id, 128, "file-abc");
    snprintf(purpose.returned[0].family, 24, "file");
    purpose.returned[0].score = 4.2;
    ma_trace_note_retrieval(log, "t2", &purpose);
    ma_trace_note_retrieval(log, "t2", &purpose);      /* the same purpose replaces itself */
    ma_trace_note_contribution(log, "t2", "1 owner, 1 document, 2 passages");
    const ma_trace_record *t2 = ma_trace_find(log, "t2");
    MARY_ASSERT_EQ(t2->purpose_count, 1);
    MARY_ASSERT_STR(t2->contribution, "1 owner, 1 document, 2 passages");
    struct json_object *json = ma_trace_json(log, &roster, 1002);
    const char *text = json_object_to_json_string_ext(json, JSON_C_TO_STRING_PLAIN);
    MARY_ASSERT_EQ(json_object_array_length(json), 2);
    MARY_ASSERT(strstr(text, "\"invocation\":\"media.next\"") && strstr(text, "\"document_id\":\"file-abc\"") && strstr(text, "\"decidedBy\":\"namedPart\""));
    json_object_put(json);
    mc_buf out = { 0 };
    MARY_ASSERT_EQ(ma_trace_report(log, &roster, 1002, &out), 0);
    text = (const char *)out.data;
    MARY_ASSERT(strncmp(text, "=== Mary ABILITY ROUTES 1970-01-01T00:16:42Z ===\nturns: 2\nability.runs: 1\nability.blocked: 0\n", 90) == 0);
    MARY_ASSERT(strstr(text, "intent.ask: 1\n") && strstr(text, "intent.converse: 1\n"));
    MARY_ASSERT(strstr(text, "--- ask via namedPart ---\nage: 1.0s\nutterance: read me the section about batteries\nlead: none\n") != NULL);
    MARY_ASSERT(strstr(text, "skill.media.next: completed | act\n") != NULL);
    MARY_ASSERT(strstr(text, "retrieval.context.lanes: personal, behavioral\nretrieval.context.returned: file-abc=4.20\n") != NULL);
    MARY_ASSERT(strstr(text, "verdict.named-part: batteries\n") != NULL);
    mc_buf_free(&out);
    ma_trace_log_free(log);
}

MARY_TEST(surfaces_and_selections_cross_the_wire) {
    struct json_object *entry = json_tokener_parse(
        "{\"place\":\"applications:textedit\",\"capturedAt\":100000,\"surface\":{\"application\":{\"name\":\"TextEdit\",\"id\":\"textedit\",\"pid\":42},"
        "\"activeWindow\":{\"title\":\"Essay\"},\"windowCount\":1,\"elements\":[{\"role\":\"AXButton\",\"kind\":\"button\",\"label\":\"Save\"},"
        "{\"role\":\"AXTextArea\",\"kind\":\"text area\",\"label\":\"body\",\"focused\":true}],\"document\":{\"name\":\"Essay\",\"path\":\"/home/mary/Essay.txt\","
        "\"text\":\"The tide comes in.\",\"total\":1200,\"lower\":40,\"upper\":58}}}");
    ma_surface s;
    MARY_ASSERT_EQ(ma_surface_parse(entry, 200, &s), 0);
    MARY_ASSERT_STR(s.app_name, "TextEdit");
    MARY_ASSERT_EQ(s.pid, 42);
    MARY_ASSERT_NEAR(s.captured_at, 100.0, 1e-9);
    MARY_ASSERT(s.has_window && strcmp(s.window_title, "Essay") == 0);
    MARY_ASSERT_EQ(s.element_count, 2);
    MARY_ASSERT_STR(s.elements[0].identity, "axbutton|Save");
    MARY_ASSERT(s.has_focused && strcmp(s.focused.label, "body") == 0);
    MARY_ASSERT_STR(s.document_path, "/home/mary/Essay.txt");
    ma_fact f;
    MARY_ASSERT_EQ(ma_surface_document_fact(entry, &s, &f), 1);
    MARY_ASSERT(f.slot.kind == MA_SLOT_FILE && f.has_bounds && f.lower == 40 && f.upper == 58 && f.document_total == 1200);
    MARY_ASSERT_STR(f.subject, "Essay");
    MARY_ASSERT_STR(f.content, "The tide comes in.");
    struct json_object *back = ma_surface_json(&s, 101, &roster);
    const char *text = json_object_to_json_string_ext(back, JSON_C_TO_STRING_PLAIN);
    MARY_ASSERT(strstr(text, "\"surfaceLine\":\"On screen: TextEdit \xE2\x80\x94 \\\"Essay\\\" (focused: body) \xE2\x80\x94 offering: body, Save \xE2\x80\x94 seen 1s ago\"") != NULL);
    json_object_put(back);
    json_object_put(entry);
    struct json_object *bad = json_tokener_parse("{\"place\":\"mac:x\"}");
    MARY_ASSERT_EQ(ma_surface_parse(bad, 0, &s), -EINVAL);
    json_object_put(bad);
    struct json_object *sel = json_tokener_parse("{\"type\":\"selection\",\"applicationID\":\"textedit\",\"text\":\"the tide\",\"document\":\"Essay\",\"lower\":4,\"upper\":12,\"total\":1200,\"editable\":true,\"capturedAt\":100000}");
    ma_selection packet;
    MARY_ASSERT_EQ(ma_selection_parse(sel, 200, &packet), 0);
    MARY_ASSERT_STR(packet.place.application, "textedit");
    MARY_ASSERT(packet.has_range && packet.editable && packet.document_total == 1200);
    MARY_ASSERT_STR(packet.subject, "Essay");
    MARY_ASSERT_NEAR(packet.captured_at, 100.0, 1e-9);
    json_object_put(sel);
}

MARY_TEST(the_ambient_state_lists_places_with_their_surfaces_and_facts) {
    ma_store *store = ma_store_new();
    ma_store_set_roster(store, &roster);
    ma_place p = ma_place_application("textedit");
    ma_surface s;
    ma_surface_init(&s, &p, "TextEdit", "textedit", 100);
    ma_store_note_surface(store, &s, 100);
    ma_fact f;
    ma_slot slot = ma_slot_of(MA_SLOT_FILE);
    ma_fact_init(&f, &p, &slot, "The tide.", MA_PROVENANCE_LIVE_AX, 100);
    ma_store_register(store, &f, 100);
    ma_focus_signal focus = { .has_lead = true, .lead = p };
    struct json_object *state = ma_ambient_state_json(store, &roster, &focus, 101);
    const char *text = json_object_to_json_string_ext(state, JSON_C_TO_STRING_PLAIN);
    MARY_ASSERT(strstr(text, "\"isLead\":true") && strstr(text, "\"mention\":\"TextEdit \xE2\x80\x94 the document in front of them, seen 1s ago.\"") && strstr(text, "\"factCount\":1"));
    json_object_put(state);
    ma_store_free(store);
}

int main(void) {
    ma_roster_maryos(&roster);
    MARY_RUN(the_ring_keeps_the_newest_fifty_newest_first);
    MARY_RUN(runs_and_retrieval_attach_to_their_turn);
    MARY_RUN(surfaces_and_selections_cross_the_wire);
    MARY_RUN(the_ambient_state_lists_places_with_their_surfaces_and_facts);
    MARY_TEST_MAIN_END();
}
