/* AmbientRanker: relevance, the three-way rule, deixis, anaphora, and rendering under a budget. */
#include "ambient/ranker.h"
#include "mary_test.h"

static ma_roster roster;

static ma_fact fact(const char *app, ma_slot_kind kind, const char *content, double at, ma_provenance p) {
    ma_fact f;
    ma_place place = ma_place_application(app);
    ma_slot slot = ma_slot_of(kind);
    ma_fact_init(&f, &place, &slot, content, p, at);
    return f;
}

MARY_TEST(tokens_drop_grammar_and_short_words) {
    char out[16][32];
    int n = ma_tokens("Read me the part about the tides again, please", out, 16);
    MARY_ASSERT_EQ(n, 2);
    MARY_ASSERT_STR(out[0], "part");
    MARY_ASSERT_STR(out[1], "tides");
}

MARY_TEST(relevance_counts_hits_and_asked_for_outranks_perceived) {
    ma_fact a = fact("textedit", MA_SLOT_FILE, "The tides come in twice a day.", 100, MA_PROVENANCE_LIVE_AX);
    ma_fact b = fact("textedit", MA_SLOT_FILE, "The tides come in twice a day.", 100, MA_PROVENANCE_LIVE_AX);
    b.registration = MA_REGISTERED_ASKED_FOR;
    double ra = ma_relevance(&a, "what about the tides", 100), rb = ma_relevance(&b, "what about the tides", 100);
    MARY_ASSERT_NEAR(ra, 3.0 + 0.75 + 1.0, 1e-9);
    MARY_ASSERT_NEAR(rb, ra + 1.5, 1e-9);
    ma_fact stale = a;
    MARY_ASSERT_NEAR(ma_relevance(&stale, "what about the tides", 160), 3.0 + 0.75 + 0.5 - 0.5, 1e-9);
}

MARY_TEST(deixis_selection_and_application_anaphora) {
    MARY_ASSERT(ma_is_deictic("what's on my screen?"));
    MARY_ASSERT(ma_is_deictic("tighten this paragraph"));
    MARY_ASSERT(ma_is_deictic("what did I highlight"));
    MARY_ASSERT(!ma_is_deictic("read the essay"));
    MARY_ASSERT(ma_references_selection("fix the passage"));
    MARY_ASSERT(ma_references_application_anaphorically("yeah can you write a poem in that window"));
    MARY_ASSERT(ma_references_application_anaphorically("where is it"));
    MARY_ASSERT(ma_references_application_anaphorically("can you add a draft here"));
    MARY_ASSERT(ma_references_application_anaphorically("do it in the same app"));
    MARY_ASSERT(!ma_references_application_anaphorically("write a poem"));
}

MARY_TEST(the_three_way_rule_over_places) {
    ma_place textedit = ma_place_application("textedit"), media = ma_place_application("media");
    MARY_ASSERT_EQ(ma_ranker_mode("what's the weather", NULL, &roster), MA_RANKING_RELEVANCE);
    MARY_ASSERT_EQ(ma_ranker_mode("what's on my screen", &textedit, &roster), MA_RANKING_FOCUSED_WORLD);
    MARY_ASSERT_EQ(ma_ranker_mode("tighten the intro in TextEdit", &textedit, &roster), MA_RANKING_FOCUSED_WORLD);
    MARY_ASSERT_EQ(ma_ranker_mode("tighten the intro in TextEdit", &media, &roster), MA_RANKING_TRANSFORM_UNFOCUSED);
    MARY_ASSERT_EQ(ma_ranker_mode("what is the Media Player playing", &textedit, &roster), MA_RANKING_RELEVANCE);
    MARY_ASSERT_EQ(ma_ranker_mode("what's the weather", &textedit, &roster), MA_RANKING_RELEVANCE);
    ma_place named[8];
    int n = ma_named_places("tighten this paragraph", &roster, named, 8);   /* the discipline cue admits every writing place with eyes */
    MARY_ASSERT_EQ(n, 1);
    MARY_ASSERT_STR(named[0].application, "textedit");
    n = ma_explicitly_named_places("open it in the Finder and the player", &roster, named, 8);
    MARY_ASSERT_EQ(n, 2);
}

MARY_TEST(rank_partitions_on_focus_and_renders_three_blocks_then_mentions) {
    ma_fact facts[5];
    facts[0] = fact("finder", MA_SLOT_VIEWPORT, "Documents: Essay.txt, Notes.txt and the tides report", 100, MA_PROVENANCE_LIVE_AX);
    facts[1] = fact("textedit", MA_SLOT_FILE, "The tides come in twice a day; the moon pulls them.", 100, MA_PROVENANCE_LIVE_AX);
    facts[2] = fact("calendar", MA_SLOT_DIGEST, "3 events today", 100, MA_PROVENANCE_DERIVED);
    facts[3] = fact("media", MA_SLOT_VIEWPORT, "Playing: Moonlight Sonata", 100, MA_PROVENANCE_LIVE_AX);
    facts[4] = fact("textedit", MA_SLOT_VIEWPORT, "paragraph 3 of the essay", 100, MA_PROVENANCE_LIVE_AX);
    ma_place textedit = ma_place_application("textedit");
    ma_fact ranked[5];
    memcpy(ranked, facts, sizeof ranked);
    ma_ranking_mode mode = ma_rank(ranked, 5, "what about the tides on my screen", &textedit, NULL, 101, &roster);
    MARY_ASSERT_EQ(mode, MA_RANKING_FOCUSED_WORLD);
    MARY_ASSERT_STR(ranked[0].place.application, "textedit");    /* focused facts lead, however relevant the others */
    MARY_ASSERT_STR(ranked[1].place.application, "textedit");
    MARY_ASSERT_STR(ranked[2].place.application, "finder");      /* then relevance: "tides" hits */
    ma_rendering *r = malloc(sizeof *r);
    ma_surface surface;
    ma_surface_init(&surface, &textedit, "TextEdit", "textedit", 100);
    snprintf(surface.window_title, sizeof surface.window_title, "Essay");
    surface.has_window = true;
    const ma_surface *surfaces[] = { &surface };
    ma_render_inputs in = { .facts = facts, .fact_count = 5, .utterance = "what about the tides on my screen", .focused = &textedit,
                            .surfaces = surfaces, .surface_count = 1, .now = 101, .roster = &roster };
    ma_render(&in, r);
    MARY_ASSERT_EQ(r->surface_count, 1);
    MARY_ASSERT(strncmp(r->surface_lines[0], "On screen: TextEdit \xE2\x80\x94 \"Essay\" \xE2\x80\x94 seen 1s ago", 60) == 0);
    /* perceived facts of other focusable places are mentions; the eyeless digest that concerns nothing is a mention */
    MARY_ASSERT_EQ(r->block_count, 2);
    MARY_ASSERT(strstr(r->blocks[0], "TextEdit \xE2\x80\x94 the document in front of them, seen 1s ago:\nThe tides come in") != NULL);
    MARY_ASSERT_EQ(r->mention_count, 3);
    MARY_ASSERT_EQ(r->key_count, 5);
    MARY_ASSERT(strstr(r->mentions[0], "Finder") || strstr(r->mentions[1], "Finder") || strstr(r->mentions[2], "Finder"));
    /* a tiny budget: the first block always fits, the rest degrade */
    in.budget = 60;
    ma_render(&in, r);
    MARY_ASSERT_EQ(r->surface_count, 1);
    MARY_ASSERT_EQ(r->block_count, 1);
    MARY_ASSERT_EQ(r->mention_count, 4);
    /* an already-rendered key is skipped */
    const char *already[] = { "applications:textedit/file" };
    in.budget = 0;
    in.already_rendered = already;
    in.already_count = 1;
    ma_render(&in, r);
    MARY_ASSERT_EQ(r->key_count, 4);
    free(r);
}

MARY_TEST(co_active_lines_name_a_place_even_with_nothing_to_carry) {
    ma_fact facts[1] = { fact("media", MA_SLOT_VIEWPORT, "Playing: Moonlight Sonata", 100, MA_PROVENANCE_LIVE_AX) };
    ma_place places[2] = { ma_place_application("media"), ma_place_application("finder") };
    bool glanced[2] = { false, true };
    char lines[2][MA_MENTION_MAX * 2], keys[4][MA_KEY_MAX];
    int key_count = 0;
    int n = ma_co_active_lines(places, glanced, 2, facts, 1, MA_CO_ACTIVE_PLACE_BUDGET, MA_CO_ACTIVE_TOTAL_BUDGET, 101, &roster, lines, 2, keys, 4, &key_count);
    MARY_ASSERT_EQ(n, 2);
    MARY_ASSERT(strncmp(lines[0], "\xE2\x80\x94 Media Player: Media Player \xE2\x80\x94 what they're looking at, seen 1s ago.", 40) == 0);
    MARY_ASSERT_STR(lines[1], "\xE2\x80\x94 Finder (glanced): you just looked at it.");
    MARY_ASSERT_EQ(key_count, 1);
    MARY_ASSERT_STR(keys[0], "applications:media/viewport");
}

int main(void) {
    ma_roster_maryos(&roster);
    MARY_RUN(tokens_drop_grammar_and_short_words);
    MARY_RUN(relevance_counts_hits_and_asked_for_outranks_perceived);
    MARY_RUN(deixis_selection_and_application_anaphora);
    MARY_RUN(the_three_way_rule_over_places);
    MARY_RUN(rank_partitions_on_focus_and_renders_three_blocks_then_mentions);
    MARY_RUN(co_active_lines_name_a_place_even_with_nothing_to_carry);
    MARY_TEST_MAIN_END();
}
