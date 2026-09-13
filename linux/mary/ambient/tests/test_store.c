/* AmbientContextStore and AmbientSurfaceStoreTests: supersession by slot, freshness and retention,
 * the named-read cap, surfaces that drop at expiry, the selection handoff and the world. */
#include "ambient/store.h"
#include "mary_test.h"

static ma_roster roster;
static ma_store *store;

static ma_fact fact_at(const char *app, ma_slot_kind kind, const char *content, double at) {
    ma_fact f;
    ma_place p = ma_place_application(app);
    ma_slot s = ma_slot_of(kind);
    ma_fact_init(&f, &p, &s, content, MA_PROVENANCE_LIVE_AX, at);
    return f;
}

MARY_TEST(slots_have_tokens_orders_and_a_perceived_axis) {
    ma_slot read = ma_slot_read("batteries", NULL), read2 = ma_slot_read("canvas", "Essay"), file = ma_slot_of(MA_SLOT_FILE);
    char token[300];
    ma_slot_token(&read, token, sizeof token);
    MARY_ASSERT_STR(token, "read:batteries");
    ma_slot_token(&read2, token, sizeof token);
    MARY_ASSERT_STR(token, "read:Essay#canvas");
    ma_slot_token(&file, token, sizeof token);
    MARY_ASSERT_STR(token, "file");
    MARY_ASSERT(ma_slot_is_perceived(&file) && !ma_slot_is_perceived(&read));
    ma_slot cursor = ma_slot_of(MA_SLOT_CURSOR), digest = ma_slot_of(MA_SLOT_DIGEST);
    MARY_ASSERT(!ma_slot_is_perceived(&cursor) && !ma_slot_is_perceived(&digest));
    MARY_ASSERT(ma_slot_order(&read) < ma_slot_order(&file));
    ma_slot viewport = ma_slot_of(MA_SLOT_VIEWPORT);
    MARY_ASSERT(ma_slot_order(&viewport) < ma_slot_order(&cursor));      /* the thing in front of their eyes beats where they typed */
}

MARY_TEST(a_fact_takes_the_windows_of_its_slot_and_provenance) {
    ma_fact f = fact_at("textedit", MA_SLOT_FILE, "Some text", 1000);
    MARY_ASSERT_NEAR(f.fresh_for, 5.0, 1e-9);
    MARY_ASSERT_NEAR(f.retain_for, 300.0, 1e-9);
    ma_fact d = fact_at("calendar", MA_SLOT_DIGEST, "3 events today", 1000);
    MARY_ASSERT_NEAR(d.fresh_for, 540.0, 1e-9);
    MARY_ASSERT_NEAR(d.retain_for, 900.0, 1e-9);
    ma_fact c = fact_at("textedit", MA_SLOT_CURSOR, "at 'the'", 1000);
    MARY_ASSERT_NEAR(c.fresh_for, 12.0, 1e-9);
    MARY_ASSERT_NEAR(c.retain_for, 60.0, 1e-9);
    ma_slot read = ma_slot_read("x", NULL);
    MARY_ASSERT_NEAR(ma_fact_default_retain_for(&read), 1200.0, 1e-9);
    MARY_ASSERT_NEAR(ma_fact_default_fresh_for(MA_PROVENANCE_CACHED_BODY, &read), 30.0, 1e-9);
    MARY_ASSERT(ma_fact_is_fresh(&f, 1004) && !ma_fact_is_fresh(&f, 1006) && !ma_fact_is_expired(&f, 1006) && ma_fact_is_expired(&f, 1301));
    MARY_ASSERT_NEAR(ma_fact_age(&f, 1010), 10.0, 1e-9);
    char key[512];
    ma_fact_key(&f, key, sizeof key);
    MARY_ASSERT_STR(key, "applications:textedit/file");
    /* the content is clipped to 2000 code points */
    char big[4096];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    ma_fact clipped = fact_at("textedit", MA_SLOT_FILE, big, 0);
    MARY_ASSERT_EQ(strlen(clipped.content), MA_CONTENT_CAP);
}

MARY_TEST(a_superseding_write_replaces_its_slot_only) {
    ma_store_clear(store);
    ma_fact a = fact_at("textedit", MA_SLOT_FILE, "first", 100), b = fact_at("textedit", MA_SLOT_FILE, "second", 101), v = fact_at("textedit", MA_SLOT_VIEWPORT, "view", 101);
    ma_store_register(store, &a, 100);
    ma_store_register(store, &v, 101);
    ma_store_register(store, &b, 101);
    MARY_ASSERT_EQ(ma_store_count(store), 2);
    ma_fact out;
    ma_place p = ma_place_application("textedit");
    ma_slot s = ma_slot_of(MA_SLOT_FILE);
    MARY_ASSERT(ma_store_fact(store, &p, &s, 102, &out));
    MARY_ASSERT_STR(out.content, "second");
    /* a selection never enters through register */
    ma_fact sel = fact_at("textedit", MA_SLOT_SELECTION, "picked", 101);
    ma_store_register(store, &sel, 101);
    MARY_ASSERT_EQ(ma_store_count(store), 2);
}

MARY_TEST(replace_perceived_wipes_one_places_perceived_slots_and_keeps_reads) {
    ma_store_clear(store);
    ma_fact f = fact_at("textedit", MA_SLOT_FILE, "doc", 100), v = fact_at("textedit", MA_SLOT_VIEWPORT, "view", 100), other = fact_at("finder", MA_SLOT_VIEWPORT, "files", 100);
    ma_fact read;
    ma_place p = ma_place_application("textedit");
    ma_slot rs = ma_slot_read("tides", NULL);
    ma_fact_init(&read, &p, &rs, "the tide comes in", MA_PROVENANCE_RECIPE_READ, 100);
    read.registration = MA_REGISTERED_ASKED_FOR;
    ma_store_register(store, &f, 100);
    ma_store_register(store, &v, 100);
    ma_store_register(store, &other, 100);
    ma_store_register(store, &read, 100);
    ma_fact fresh = fact_at("textedit", MA_SLOT_FILE, "doc v2", 105);
    ma_store_replace_perceived(store, &p, &fresh, 1, 105);
    MARY_ASSERT_EQ(ma_store_count(store), 3);       /* doc v2, finder's viewport, the read */
    ma_fact out;
    ma_slot vs = ma_slot_of(MA_SLOT_VIEWPORT);
    MARY_ASSERT(!ma_store_fact(store, &p, &vs, 105, &out));
    MARY_ASSERT(ma_store_fact(store, &p, &rs, 105, &out) && out.registration == MA_REGISTERED_ASKED_FOR);
    ma_store_forget_perceived(store, &p);
    MARY_ASSERT_EQ(ma_store_count(store), 2);
    MARY_ASSERT(ma_store_fact(store, &p, &rs, 105, &out));
    ma_store_forget_attention(store, MA_ATTENTION_APPLICATIONS);
    MARY_ASSERT_EQ(ma_store_count(store), 0);
}

MARY_TEST(the_newest_four_reads_per_place_survive) {
    ma_store_clear(store);
    ma_place p = ma_place_application("textedit"), q = ma_place_application("preview");
    for (int i = 0; i < 6; i++) {
        char phrase[16];
        snprintf(phrase, sizeof phrase, "phrase%d", i);
        ma_fact f;
        ma_slot s = ma_slot_read(phrase, NULL);
        ma_fact_init(&f, i == 5 ? &q : &p, &s, "text", MA_PROVENANCE_RECIPE_READ, 100 + i);
        ma_store_register(store, &f, 100 + i);
    }
    ma_fact reads[16];
    int n = ma_store_reads(store, 110, reads, 16);
    MARY_ASSERT_EQ(n, 5);       /* four on textedit, one on preview: the cap is per place */
    ma_slot oldest = ma_slot_read("phrase0", NULL);
    MARY_ASSERT(!ma_store_fact(store, &p, &oldest, 110, NULL));
}

MARY_TEST(facts_expire_and_are_ordered_by_place_slot_and_key) {
    ma_store_clear(store);
    ma_fact late = fact_at("textedit", MA_SLOT_FILE, "old", 0), view = fact_at("finder", MA_SLOT_VIEWPORT, "files", 100), digest = fact_at("calendar", MA_SLOT_DIGEST, "3 events", 100);
    ma_store_register(store, &late, 0);
    ma_store_register(store, &view, 100);
    ma_store_register(store, &digest, 100);
    ma_fact out[8];
    int n = ma_store_facts(store, 400, out, 8);
    MARY_ASSERT_EQ(n, 2);           /* "old" is past its 300 s retention */
    /* roster order: finder (index 1) before calendar (index 3) */
    MARY_ASSERT_STR(out[0].place.application, "finder");
    MARY_ASSERT_STR(out[1].place.application, "calendar");
}

MARY_TEST(surfaces_drop_at_expiry_and_the_latest_capture_wins) {
    ma_store_clear(store);
    ma_place p = ma_place_application("textedit");
    ma_surface a, b, c;
    ma_surface_init(&a, &p, "TextEdit", "textedit", 100);
    ma_surface_init(&b, &p, "TextEdit", "textedit", 90);      /* out of order */
    ma_surface_init(&c, &p, "TextEdit", "textedit", 110);
    snprintf(a.window_title, sizeof a.window_title, "A");
    snprintf(b.window_title, sizeof b.window_title, "B");
    snprintf(c.window_title, sizeof c.window_title, "C");
    a.has_window = b.has_window = c.has_window = true;
    ma_store_note_surface(store, &a, 100);
    ma_store_note_surface(store, &b, 101);
    MARY_ASSERT_STR(ma_store_surface(store, &p, 101)->window_title, "A");
    ma_store_note_surface(store, &c, 111);
    MARY_ASSERT_STR(ma_store_surface(store, &p, 111)->window_title, "C");
    MARY_ASSERT(ma_store_surface(store, &p, 136) == NULL);      /* 25 s later it is gone, not stale */
    ma_surface f;
    ma_place fp = ma_place_application("finder");
    ma_surface_init(&f, &fp, "Finder", "finder", 200);
    ma_surface_init(&c, &p, "TextEdit", "textedit", 200);
    ma_store_note_surface(store, &c, 200);
    ma_store_note_surface(store, &f, 200);
    const ma_surface *all[4];
    MARY_ASSERT_EQ(ma_store_surfaces(store, 201, all, 4), 2);
    MARY_ASSERT_STR(all[0]->app_id, "textedit");     /* roster order */
    MARY_ASSERT_STR(all[1]->app_id, "finder");
}

MARY_TEST(the_selection_is_a_thirty_second_packet_a_late_read_cannot_revive) {
    ma_store_clear(store);
    ma_selection sel = { .place = ma_place_application("textedit"), .captured_at = 100 };
    snprintf(sel.application_id, sizeof sel.application_id, "textedit");
    snprintf(sel.text, sizeof sel.text, "the chosen words");
    snprintf(sel.subject, sizeof sel.subject, "Essay");
    ma_store_record_selection(store, &sel, 100);
    ma_selection out;
    MARY_ASSERT(ma_store_selection(store, 120, &out));
    MARY_ASSERT(!ma_store_selection(store, 131, &out));
    /* it projects as a fact and as the world */
    ma_fact facts[4];
    MARY_ASSERT_EQ(ma_store_facts(store, 110, facts, 4), 1);
    MARY_ASSERT(facts[0].slot.kind == MA_SLOT_SELECTION && facts[0].has_bounds == false);
    MARY_ASSERT_STR(facts[0].subject, "Essay");
    ma_world w;
    MARY_ASSERT(ma_store_world(store, 110, &w));
    MARY_ASSERT(ma_world_is_direct_reference(&w) && ma_world_matches(&w, &facts[0]));
    MARY_ASSERT_STR(w.selected_text, "the chosen words");
    /* cleared: a capture from before the clear is refused; a later one is taken */
    ma_store_clear_selection(store, "textedit", 115);
    MARY_ASSERT(!ma_store_selection(store, 116, &out));
    ma_store_record_selection(store, &sel, 116);
    MARY_ASSERT(!ma_store_selection(store, 116, &out));
    sel.captured_at = 118;
    ma_store_record_selection(store, &sel, 118);
    MARY_ASSERT(ma_store_selection(store, 119, &out));
    /* a turn claims it once */
    MARY_ASSERT(ma_store_claim_selection(store, 119, &out));
    MARY_ASSERT(!ma_store_selection(store, 120, &out));
    sel.captured_at = 121;
    snprintf(sel.id, sizeof sel.id, "sel-2");
    ma_store_record_selection(store, &sel, 121);
    MARY_ASSERT(ma_store_selection(store, 122, &out));
}

MARY_TEST(the_world_snapshot_keeps_the_newer_capture_and_fades) {
    ma_store_clear(store);
    ma_world a = { .sense = MA_SENSE_WORKSPACE, .attention = MA_ATTENTION_APPLICATIONS, .captured_at = 100, .fresh_for = 15 };
    snprintf(a.application_id, sizeof a.application_id, "finder");
    ma_world b = a;
    b.captured_at = 95;
    snprintf(b.application_id, sizeof b.application_id, "media");
    ma_store_note_world(store, &a, 100);
    ma_store_note_world(store, &b, 101);
    ma_world out;
    MARY_ASSERT(ma_store_world(store, 101, &out));
    MARY_ASSERT_STR(out.application_id, "finder");
    MARY_ASSERT(!ma_store_world(store, 116, &out));
    ma_world sel = { .sense = MA_SENSE_SELECTION, .captured_at = 200, .fresh_for = 30 };
    ma_store_note_world(store, &sel, 200);          /* selections go through record_selection only */
    MARY_ASSERT(!ma_store_world(store, 200, &out));
}

MARY_TEST(the_lead_decays_and_spoken_notes_land_by_containment) {
    ma_store_clear(store);
    ma_place p = ma_place_application("textedit");
    ma_store_note_lead(store, &p, 100);
    ma_place out;
    MARY_ASSERT(ma_store_lead_place(store, 399, &out) && ma_place_equal(&out, &p));
    MARY_ASSERT(!ma_store_lead_place(store, 401, &out));
    ma_fact f = fact_at("textedit", MA_SLOT_FILE, "the tide comes in", 100);
    ma_store_register(store, &f, 100);
    const char *passages[] = { "She said: the tide comes in, then goes out." };
    MARY_ASSERT_EQ(ma_store_note_spoken(store, passages, 1, "spoke about the tide", 101), 1);
    ma_fact got;
    ma_slot s = ma_slot_of(MA_SLOT_FILE);
    MARY_ASSERT(ma_store_fact(store, &p, &s, 101, &got) && got.spoken_at == 101);
    MARY_ASSERT_STR(got.spoken_note, "spoke about the tide");
    ma_store_note_utterance(store, "hello");
    MARY_ASSERT_STR(ma_store_utterance(store), "hello");
}

int main(void) {
    ma_roster_maryos(&roster);
    store = ma_store_new();
    ma_store_set_roster(store, &roster);
    MARY_RUN(slots_have_tokens_orders_and_a_perceived_axis);
    MARY_RUN(a_fact_takes_the_windows_of_its_slot_and_provenance);
    MARY_RUN(a_superseding_write_replaces_its_slot_only);
    MARY_RUN(replace_perceived_wipes_one_places_perceived_slots_and_keeps_reads);
    MARY_RUN(the_newest_four_reads_per_place_survive);
    MARY_RUN(facts_expire_and_are_ordered_by_place_slot_and_key);
    MARY_RUN(surfaces_drop_at_expiry_and_the_latest_capture_wins);
    MARY_RUN(the_selection_is_a_thirty_second_packet_a_late_read_cannot_revive);
    MARY_RUN(the_world_snapshot_keeps_the_newer_capture_and_fades);
    MARY_RUN(the_lead_decays_and_spoken_notes_land_by_containment);
    ma_store_free(store);
    MARY_TEST_MAIN_END();
}
