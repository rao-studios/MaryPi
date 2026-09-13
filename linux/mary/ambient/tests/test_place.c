/* AmbientPlaceTests and the roster: tokens, the inverse, names, and what the words ask for. */
#include "ambient/place.h"
#include "mary_test.h"
#include <json-c/json.h>

static ma_roster roster;

MARY_TEST(tokens_are_the_swift_spelling) {
    ma_place textedit = ma_place_application("textedit"), mac = ma_place_lane(MA_ATTENTION_MAC), wm = ma_place_lane(MA_ATTENTION_WINDOW_MANAGEMENT);
    char token[MA_TOKEN_MAX];
    ma_place_token(&textedit, token, sizeof token);
    MARY_ASSERT_STR(token, "applications:textedit");
    MARY_ASSERT_STR(ma_place_memory_token(&textedit), "textedit");
    ma_place_token(&mac, token, sizeof token);
    MARY_ASSERT_STR(token, "mac");
    ma_place_token(&wm, token, sizeof token);
    MARY_ASSERT_STR(token, "window-management");
    MARY_ASSERT_STR(ma_place_memory_token(&wm), "window-management");
    MARY_ASSERT(ma_place_is_application(&textedit) && !ma_place_is_application(&mac));
    MARY_ASSERT(textedit.attention == MA_ATTENTION_APPLICATIONS);
}

MARY_TEST(from_token_splits_on_the_first_colon_and_only_applications_spells_dynamics) {
    ma_place p;
    MARY_ASSERT(ma_place_from_token("applications:media", &p));
    MARY_ASSERT_STR(p.application, "media");
    MARY_ASSERT(ma_place_from_token("typer", &p) && !ma_place_is_application(&p) && p.attention == MA_ATTENTION_TYPER);
    MARY_ASSERT(!ma_place_from_token("mac:finder", &p));
    MARY_ASSERT(!ma_place_from_token("applications:", &p));
    MARY_ASSERT(!ma_place_from_token("pages", &p));
    MARY_ASSERT(!ma_place_from_token("", &p));
    ma_place a = ma_place_application("finder"), b = ma_place_application("media");
    MARY_ASSERT(ma_place_compare(&a, &b) < 0);
    MARY_ASSERT(ma_place_equal(&a, &a) && !ma_place_equal(&a, &b));
}

MARY_TEST(the_roster_names_places_and_says_who_has_eyes) {
    ma_place textedit = ma_place_application("textedit"), desktop = ma_place_application("desktop"), mac = ma_place_lane(MA_ATTENTION_MAC), unknown = ma_place_application("sketch");
    MARY_ASSERT_STR(ma_place_display_name(&textedit, &roster), "TextEdit");
    MARY_ASSERT_STR(ma_place_display_name(&mac, &roster), "Mac");
    MARY_ASSERT_STR(ma_place_display_name(&unknown, &roster), "sketch");
    MARY_ASSERT(ma_place_has_eyes(&textedit, &roster) && !ma_place_has_eyes(&desktop, &roster) && !ma_place_has_eyes(&mac, &roster));
    MARY_ASSERT_STR(ma_place_focus(&textedit, &roster), "writing");
    MARY_ASSERT(ma_place_focus(&desktop, &roster) == NULL);          /* window management is no discipline */
    MARY_ASSERT_STR(ma_place_ability(&desktop, &roster), "window-management");
    MARY_ASSERT_STR(ma_place_class_name(&textedit, &roster), "workspace");
    MARY_ASSERT_STR(ma_place_class_name(&desktop, &roster), "service");
    MARY_ASSERT_STR(ma_place_class_name(&mac, &roster), "service");
    MARY_ASSERT(ma_place_order(&mac, &roster) == 1);
    MARY_ASSERT(ma_place_order(&textedit, &roster) >= 1000 && ma_place_order(&textedit, &roster) < ma_place_order(&desktop, &roster));
}

MARY_TEST(mentions_are_whole_word_alias_runs) {
    const ma_registration *media = ma_roster_registration(&roster, "media"), *textedit = ma_roster_registration(&roster, "textedit");
    MARY_ASSERT(ma_registration_is_mentioned(media, "open the Media Player"));
    MARY_ASSERT(ma_registration_is_mentioned(media, "put it in the player"));
    MARY_ASSERT(!ma_registration_is_mentioned(media, "the mediator was late"));
    MARY_ASSERT(ma_registration_is_mentioned(textedit, "in TextEdit please"));
    MARY_ASSERT(ma_registration_is_mentioned(textedit, "in text edit please"));
    MARY_ASSERT(!ma_registration_is_mentioned(textedit, "edit the text"));
}

MARY_TEST(the_words_ask_for_abilities_and_name_a_discipline) {
    char ids[4][32];
    int n = ma_roster_requested_abilities(&roster, "play the next song and tighten the intro", ids, 4);
    MARY_ASSERT_EQ(n, 2);
    MARY_ASSERT_STR(ids[0], "multimedia");
    MARY_ASSERT_STR(ids[1], "writing");
    MARY_ASSERT_EQ(ma_roster_requested_abilities(&roster, "how was your day", ids, 4), 0);
    MARY_ASSERT_STR(ma_roster_discipline_in(&roster, "tighten this paragraph"), "writing");
    MARY_ASSERT(ma_roster_discipline_in(&roster, "play the song and write a note") == NULL);    /* a tie defers to window truth */
    MARY_ASSERT(ma_roster_discipline_in(&roster, "how was your day") == NULL);
    MARY_ASSERT(ma_roster_names_transform(&roster, "could you tighten the intro"));
    MARY_ASSERT(ma_roster_names_transform(&roster, "please add"));                  /* "add" at the very end matches too */
    MARY_ASSERT(!ma_roster_names_transform(&roster, "read me the intro"));
    MARY_ASSERT(ma_roster_is_discipline(&roster, "writing") && !ma_roster_is_discipline(&roster, "scheduling"));
    MARY_ASSERT_EQ(ma_roster_paradigm(&roster, "system"), MA_PARADIGM_SYSTEM_CONTROL);
    MARY_ASSERT_EQ(ma_roster_paradigm(&roster, "nothing"), MA_PARADIGM_APPLICATION_EXPERTISE);
}

MARY_TEST(the_desktops_skills_rename_and_add_registrations) {
    struct json_object *apps = json_tokener_parse("[{\"id\":\"textedit\",\"name\":\"Text Editor\"},{\"id\":\"sketch\",\"name\":\"Sketch\"}]");
    ma_roster r;
    ma_roster_maryos(&r);
    int before = r.app_count;
    ma_roster_merge_skills(&r, apps);
    json_object_put(apps);
    MARY_ASSERT_EQ(r.app_count, before + 1);
    MARY_ASSERT_STR(ma_roster_registration(&r, "textedit")->name, "Text Editor");
    const ma_registration *sketch = ma_roster_registration(&r, "sketch");
    MARY_ASSERT(sketch && !sketch->has_eyes && sketch->ability_count == 0);
    ma_place p = ma_place_application("sketch");
    MARY_ASSERT_STR(ma_place_display_name(&p, &r), "Sketch");
}

MARY_TEST(words_split_on_anything_but_letters_and_digits) {
    char words[16][32];
    int n = ma_words("What's on-screen, Mary?", words, 16, false);
    MARY_ASSERT_EQ(n, 5);
    MARY_ASSERT_STR(words[0], "what");
    MARY_ASSERT_STR(words[1], "s");
    MARY_ASSERT_STR(words[3], "screen");
    n = ma_words("What's on-screen", words, 16, true);
    MARY_ASSERT_EQ(n, 3);
    MARY_ASSERT_STR(words[0], "what's");
}

int main(void) {
    ma_roster_maryos(&roster);
    MARY_RUN(tokens_are_the_swift_spelling);
    MARY_RUN(from_token_splits_on_the_first_colon_and_only_applications_spells_dynamics);
    MARY_RUN(the_roster_names_places_and_says_who_has_eyes);
    MARY_RUN(mentions_are_whole_word_alias_runs);
    MARY_RUN(the_words_ask_for_abilities_and_name_a_discipline);
    MARY_RUN(the_desktops_skills_rename_and_add_registrations);
    MARY_RUN(words_split_on_anything_but_letters_and_digits);
    MARY_TEST_MAIN_END();
}
