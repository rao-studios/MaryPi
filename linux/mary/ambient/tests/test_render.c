/* AmbientSurfaceRenderTests and AmbientFact+Rendering: one phrasing for the prompt and the pane. */
#include "ambient/prompt.h"
#include "ambient/render.h"
#include "common/buf.h"
#include "mary_test.h"

static ma_roster roster;

MARY_TEST(ages_are_coarse) {
    char out[32];
    ma_age_string(8, out, sizeof out);
    MARY_ASSERT_STR(out, "8s");
    ma_age_string(89.4, out, sizeof out);
    MARY_ASSERT_STR(out, "89s");
    ma_age_string(150, out, sizeof out);
    MARY_ASSERT_STR(out, "3m");     /* 2.5 rounds to 3 */
    ma_age_string(7200, out, sizeof out);
    MARY_ASSERT_STR(out, "2h");
    ma_age_string(-1, out, sizeof out);
    MARY_ASSERT_STR(out, "an unknown time");
    ma_age_string(86400.0 * 31, out, sizeof out);
    MARY_ASSERT_STR(out, "an unknown time");
}

MARY_TEST(the_surface_line_names_at_most_five_and_counts_the_rest) {
    ma_place p = ma_place_application("textedit");
    ma_surface s;
    ma_surface_init(&s, &p, "TextEdit", "textedit", 100);
    s.has_window = true;
    snprintf(s.window_title, sizeof s.window_title, "Kohinoor Essay");
    s.window_count = 2;
    s.minimized_count = 1;
    for (int i = 0; i < 43; i++) {
        ma_element *e = &s.elements[s.element_count++];
        memset(e, 0, sizeof *e);
        e->ordinal = i;
        snprintf(e->kind, sizeof e->kind, "button");
        if (i < 6) snprintf(e->label, sizeof e->label, "%s", (const char *[]){ "Share", "Add Page", "Zoom", "Bold", "Italic", "Underline" }[i]);
    }
    s.has_focused = true;
    s.focused = s.elements[3];
    snprintf(s.focused.label, sizeof s.focused.label, "body text");
    s.focused.ordinal = 99;
    char line[MA_SURFACE_LINE_MAX];
    ma_surface_line(&s, 108, line, sizeof line);
    MARY_ASSERT_STR(line, "On screen: TextEdit \xE2\x80\x94 \"Kohinoor Essay\" (front of 2 windows, 1 minimized; focused: body text) "
                          "\xE2\x80\x94 offering: body text, Share, Add Page, Zoom, Bold, +38 more \xE2\x80\x94 seen 8s ago");
    s.page_not_yet_read = true;
    s.element_count = 0;
    s.has_focused = false;
    ma_surface_line(&s, 108, line, sizeof line);
    MARY_ASSERT_STR(line, "On screen: TextEdit \xE2\x80\x94 \"Kohinoor Essay\" (front of 2 windows, 1 minimized) \xE2\x80\x94 page not yet read \xE2\x80\x94 seen 8s ago");
    /* the cap: 220 code points, then an ellipsis */
    memset(s.window_title, 'x', 250);
    s.window_title[250] = 0;
    ma_surface_line(&s, 108, line, sizeof line);
    MARY_ASSERT_EQ(ma_utf8_count(line), 220);
    MARY_ASSERT(strcmp(line + strlen(line) - 3, "\xE2\x80\xA6") == 0);
}

MARY_TEST(mentions_carry_bounds_and_ages_and_blocks_add_the_text) {
    ma_place p = ma_place_application("textedit");
    ma_slot slot = ma_slot_read("tides", NULL);
    ma_fact f;
    ma_fact_init(&f, &p, &slot, "The tide comes in twice a day.", MA_PROVENANCE_RECIPE_READ, 100);
    f.registration = MA_REGISTERED_ASKED_FOR;
    snprintf(f.subject, sizeof f.subject, "Essay");
    f.has_bounds = true;
    f.lower = 40;
    f.upper = 900;
    f.document_total = 1200;
    snprintf(f.passage_handle, sizeof f.passage_handle, "S1");
    char line[MA_MENTION_MAX], block[MA_BLOCK_MAX];
    ma_fact_mention_line(&f, 105, &roster, line, sizeof line);
    MARY_ASSERT_STR(line, "[S1] TextEdit \xC2\xB7 Essay \xE2\x80\x94 the part about \"tides\", characters 40\xE2\x80\x93" "900 of 1200, read 5s ago.");
    f.spoken_at = 104;
    ma_fact_mention_line(&f, 140, &roster, line, sizeof line);
    MARY_ASSERT(strstr(line, "read 40s ago, so it may have moved on since; already spoken about.") != NULL);
    f.spoken_at = 0;
    ma_fact_block(&f, 105, &roster, 400, block, sizeof block);
    MARY_ASSERT_STR(block, "[S1] TextEdit \xC2\xB7 Essay \xE2\x80\x94 the part about \"tides\", characters 40\xE2\x80\x93" "900 of 1200, read 5s ago:\nThe tide comes in twice a day.");
    ma_fact_block(&f, 105, &roster, 100, block, sizeof block);
    MARY_ASSERT(ma_utf8_count(block) <= 100 && strstr(block, "\xE2\x80\xA6") != NULL);
    /* the three tiers of bounds */
    char bounds[96];
    f.has_bounds = false;
    ma_fact_bounds_phrase(&f, bounds, sizeof bounds);
    MARY_ASSERT_STR(bounds, "about 30 characters of 1200");
    f.document_total = 0;
    f.has_bounds = true;
    ma_fact_bounds_phrase(&f, bounds, sizeof bounds);
    MARY_ASSERT_STR(bounds, "characters 40\xE2\x80\x93" "900");
    f.has_bounds = false;
    ma_fact_bounds_phrase(&f, bounds, sizeof bounds);
    MARY_ASSERT_STR(bounds, "");
    /* a digest carries its own words and never a second body */
    ma_slot d = ma_slot_of(MA_SLOT_DIGEST);
    ma_place cal = ma_place_application("calendar");
    ma_fact digest;
    ma_fact_init(&digest, &cal, &d, "3 events today", MA_PROVENANCE_DERIVED, 100);
    ma_fact_mention_line(&digest, 101, &roster, line, sizeof line);
    MARY_ASSERT_STR(line, "Calendar \xE2\x80\x94 how things stand right now, seen 1s ago: 3 events today");
    ma_fact_block(&digest, 101, &roster, 400, block, sizeof block);
    MARY_ASSERT_STR(block, line);
    char phrase[64];
    ma_slot sel = ma_slot_of(MA_SLOT_SELECTION), file = ma_slot_of(MA_SLOT_FILE);
    f.slot = sel;
    ma_fact_slot_phrase(&f, &roster, phrase, sizeof phrase);
    MARY_ASSERT_STR(phrase, "their highlight");
    f.slot = file;
    ma_fact_slot_phrase(&f, &roster, phrase, sizeof phrase);
    MARY_ASSERT_STR(phrase, "the document in front of them");
}

MARY_TEST(the_live_work_section_names_the_place_and_lands_the_held_facts) {
    ma_rendering *r = calloc(1, sizeof *r);
    mc_buf out = { 0 };
    ma_live_work_world world = { .kind = MA_LIVE_WORK_UNLED };
    MARY_ASSERT_EQ(ma_prompt_live_work(r, &world, false, &out), 0);
    MARY_ASSERT_EQ(out.len, 0);           /* nothing in hand: nothing said */
    snprintf(r->surface_lines[r->surface_count++], MA_SURFACE_LINE_MAX, "On screen: TextEdit \xE2\x80\x94 seen 1s ago");
    snprintf(r->blocks[r->block_count++], MA_BLOCK_MAX, "TextEdit \xE2\x80\x94 the document in front of them, seen 1s ago:\nThe tide comes in.");
    snprintf(r->mentions[r->mention_count++], MA_MENTION_MAX, "Finder \xE2\x80\x94 what they're looking at, seen 3s ago.");
    world.kind = MA_LIVE_WORK_APPLICATION;
    snprintf(world.name, sizeof world.name, "TextEdit");
    MARY_ASSERT_EQ(ma_prompt_live_work(r, &world, false, &out), 0);
    const char *text = (const char *)out.data;
    static const char HEAD[] = "\n\nYou can see what the user is looking at right now \xE2\x80\x94 this is the live TextEdit window open in front of them, and it is the ground truth";
    MARY_ASSERT(strncmp(text, HEAD, strlen(HEAD)) == 0);
    MARY_ASSERT(strstr(text, "it is TextEdit, and calling it anything else") != NULL);
    const char *surface = strstr(text, "On screen: TextEdit"), *held = strstr(text, "I am also still holding these"), *block = strstr(text, "The tide comes in."), *mentions = strstr(text, "Also still held."), *mention = strstr(text, "\n- Finder");
    MARY_ASSERT(surface && held && block && mentions && mention && surface < held && held < block && block < mentions && mentions < mention);
    mc_buf_free(&out);
    world.kind = MA_LIVE_WORK_DOCUMENT;
    MARY_ASSERT_EQ(ma_prompt_live_work(r, &world, false, &out), 0);
    MARY_ASSERT(strstr((const char *)out.data, "the live document open in front of them in TextEdit") != NULL);
    MARY_ASSERT(strstr((const char *)out.data, "What you are shown is a WINDOW onto their work") != NULL);
    mc_buf_free(&out);
    free(r);
    char line[512];
    ma_place textedit = ma_place_application("textedit"), media = ma_place_application("media"), mac = ma_place_lane(MA_ATTENTION_MAC);
    ma_capability_line(&textedit, &roster, line, sizeof line);
    MARY_ASSERT(strncmp(line, "Right now you're co-writing with the user in TextEdit", 50) == 0);
    ma_capability_line(&media, &roster, line, sizeof line);
    MARY_ASSERT(strncmp(line, "Right now you're working alongside the user in Media Player", 55) == 0);
    ma_capability_line(&mac, &roster, line, sizeof line);
    MARY_ASSERT_STR(line, "");
    ma_live_work_world w;
    ma_surface s;
    ma_surface_init(&s, &textedit, "TextEdit", "textedit", 0);
    ma_live_work_from_surface(&s, &roster, &w);
    MARY_ASSERT_EQ(w.kind, MA_LIVE_WORK_APPLICATION);
    snprintf(s.document_path, sizeof s.document_path, "/home/mary/Essay.txt");
    ma_live_work_from_surface(&s, &roster, &w);
    MARY_ASSERT_EQ(w.kind, MA_LIVE_WORK_DOCUMENT);
    MARY_ASSERT_STR(w.name, "TextEdit");
}

int main(void) {
    ma_roster_maryos(&roster);
    MARY_RUN(ages_are_coarse);
    MARY_RUN(the_surface_line_names_at_most_five_and_counts_the_rest);
    MARY_RUN(mentions_carry_bounds_and_ages_and_blocks_add_the_text);
    MARY_RUN(the_live_work_section_names_the_place_and_lands_the_held_facts);
    MARY_TEST_MAIN_END();
}
