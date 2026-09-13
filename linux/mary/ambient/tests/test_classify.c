/* EditIntentClassifier, NamedPartClassifier and the intent gate's question forms, on the
 * sentences the Swift tests pin. */
#include "ambient/classify.h"
#include "mary_test.h"

static const char *const ALIASES[] = { "textedit", "sketch" };

static bool edit(const char *utterance, ma_edit_intent *out) { return ma_edit_intent_in(utterance, ALIASES, 2, out); }

MARY_TEST(replace_splits_target_from_payload_non_greedily) {
    ma_edit_intent e;
    MARY_ASSERT(edit("replace the intro with a shorter one", &e));
    MARY_ASSERT_EQ(e.shape, MA_EDIT_REPLACE);
    MARY_ASSERT_STR(e.target[0], "intro");
    MARY_ASSERT_STR(e.payload, "a shorter one");
    MARY_ASSERT(edit("change the sentence about tides to say the tide is low", &e));
    MARY_ASSERT_STR(e.target[0], "tides");
    MARY_ASSERT_STR(e.payload, "the tide is low");
    /* "for" is a split word: the courtesy tail goes first, so "me" is never the payload */
    MARY_ASSERT(edit("reword the whole thing for me", &e));
    MARY_ASSERT_EQ(e.shape, MA_EDIT_REPLACE);
    MARY_ASSERT(e.payload[0] == 0);
    MARY_ASSERT_STR(e.target[0], "whole thing");
    /* no split word: the whole tail is the target and the model writes the replacement */
    MARY_ASSERT(edit("tighten the intro", &e));
    MARY_ASSERT_EQ(e.target_count, 1);
    MARY_ASSERT_STR(e.target[0], "intro");
}

MARY_TEST(anaphora_confirmations_and_request_frames_are_peeled) {
    ma_edit_intent e;
    MARY_ASSERT(edit("tighten it up", &e));
    MARY_ASSERT(e.anaphoric && e.target_count == 0);
    MARY_ASSERT(edit("yeah that's the one, can you please polish that paragraph a bit", &e));
    MARY_ASSERT(e.anaphoric);
    MARY_ASSERT(edit("Hey Mary could you tighten the intro", &e));
    MARY_ASSERT_STR(e.target[0], "intro");
    MARY_ASSERT(edit("textedit can you fix the Purpose section", &e));
    MARY_ASSERT_STR(e.target[0], "Purpose");         /* the heading form leads */
    MARY_ASSERT_STR(e.target[1], "Purpose section");
    char stripped[256];
    ma_edit_strip_preamble("okay, that's it: delete the footnote", ALIASES, 2, stripped, sizeof stripped);
    MARY_ASSERT_STR(stripped, ": delete the footnote");
}

MARY_TEST(questions_and_addresses_to_her_are_not_edits) {
    ma_edit_intent e;
    MARY_ASSERT(!edit("what did you replace?", &e));
    MARY_ASSERT(!edit("update me on the build", &e));
    MARY_ASSERT(!edit("delete the milk reminder", &e));                    /* the eyeless veto */
    MARY_ASSERT(!edit("remove the three o'clock event from my calendar", &e));
    MARY_ASSERT(!edit("how was your day", &e));
    MARY_ASSERT(edit("can you tighten the intro?", &e));                   /* a polite frame survives its question mark */
}

MARY_TEST(delete_insert_and_move_have_their_shapes) {
    ma_edit_intent e;
    MARY_ASSERT(edit("get rid of the intro", &e));
    MARY_ASSERT_EQ(e.shape, MA_EDIT_DELETE);
    MARY_ASSERT_STR(e.target[0], "intro");
    MARY_ASSERT(edit("add a summary after the conclusion", &e));
    MARY_ASSERT_EQ(e.shape, MA_EDIT_INSERT);
    MARY_ASSERT_EQ(e.anchor, MA_ANCHOR_AFTER);
    MARY_ASSERT_STR(e.payload, "a summary");
    MARY_ASSERT_STR(e.target[0], "conclusion");
    MARY_ASSERT(edit("put a summary in place of the intro", &e));
    MARY_ASSERT_EQ(e.shape, MA_EDIT_REPLACE);
    MARY_ASSERT_STR(e.payload, "a summary");
    MARY_ASSERT(edit("insert a title at the top of the essay", &e));
    MARY_ASSERT_EQ(e.anchor, MA_ANCHOR_BEFORE);
    MARY_ASSERT(!edit("write a poem in a new document", &e));               /* composition into a fresh surface */
    MARY_ASSERT(!edit("add a draft here", &e));
    MARY_ASSERT(edit("move the conclusion before the intro", &e));
    MARY_ASSERT_EQ(e.shape, MA_EDIT_MOVE);
    MARY_ASSERT_STR(e.target[0], "conclusion");
    MARY_ASSERT_STR(e.destination[0], "intro");
    MARY_ASSERT_EQ(e.anchor, MA_ANCHOR_BEFORE);
    MARY_ASSERT(!edit("move the file to the desktop", &e) || e.shape == MA_EDIT_MOVE);
    /* clause by clause: the edit in the second clause is found */
    MARY_ASSERT(edit("read me the intro and then tighten the conclusion", &e));
    MARY_ASSERT_EQ(e.shape, MA_EDIT_REPLACE);
    MARY_ASSERT_STR(e.target[0], "conclusion");
    MARY_ASSERT(ma_edit_is_fresh_surface_phrase("a brand new Pages document") && ma_edit_is_fresh_surface_phrase("this blank page") && !ma_edit_is_fresh_surface_phrase("the intro"));
}

MARY_TEST(the_named_part_is_the_phrase_to_hand_a_read) {
    char part[128];
    MARY_ASSERT(ma_named_part("read me the part about batteries", part, sizeof part));
    MARY_ASSERT_STR(part, "batteries");
    MARY_ASSERT(ma_named_part("what does the paragraph about synthetic media say", part, sizeof part));
    MARY_ASSERT_STR(part, "synthetic media");
    MARY_ASSERT(ma_named_part("the implementation section that starts with I built a minimum viable product", part, sizeof part));
    MARY_ASSERT_STR(part, "I built a minimum viable");
    MARY_ASSERT(ma_named_part("what does it say about tides", part, sizeof part));
    MARY_ASSERT_STR(part, "tides");
    MARY_ASSERT(ma_named_part("what does the design doc say about caching?", part, sizeof part));
    MARY_ASSERT_STR(part, "caching");
    MARY_ASSERT(ma_named_part("read section five again", part, sizeof part));
    MARY_ASSERT_STR(part, "section 5");
    MARY_ASSERT(ma_named_part("go to Chapter 3.2 please", part, sizeof part));
    MARY_ASSERT_STR(part, "Chapter 3.2");
    MARY_ASSERT(!ma_named_part("the part about it", part, sizeof part));
    MARY_ASSERT(!ma_named_part("what's item 3 on my shopping list", part, sizeof part));
    MARY_ASSERT(!ma_named_part("what's on my calendar", part, sizeof part));
    MARY_ASSERT(!ma_named_part("how are you", part, sizeof part));
    MARY_ASSERT(ma_names_ambient_source("what's on my calendar tomorrow") && !ma_names_ambient_source("the tide comes in"));
    char cleaned[128];
    MARY_ASSERT(ma_named_part_clean("the paragraph, about tides", cleaned, sizeof cleaned));
    MARY_ASSERT_STR(cleaned, "paragraph");
    MARY_ASSERT(ma_named_part_clean("\xE2\x80\x9C" "moon tides\xE2\x80\x9D.", cleaned, sizeof cleaned));
    MARY_ASSERT_STR(cleaned, "moon tides");
    MARY_ASSERT(ma_named_part_clean("the tides and the moons", cleaned, sizeof cleaned));    /* a clause break ends the phrase */
    MARY_ASSERT_STR(cleaned, "tides");
}

MARY_TEST(question_forms_and_bare_decisions) {
    unsigned q = ma_question_forms("How do I know which one is where?");
    MARY_ASSERT(q == (MA_Q_HOW | MA_Q_WHICH | MA_Q_WHERE));
    MARY_ASSERT_EQ(ma_question_forms("play it"), 0);
    MARY_ASSERT_STR(ma_question_name(MA_Q_WHO), "who");
    MARY_ASSERT_EQ(ma_bare_decision("yes"), 1);
    MARY_ASSERT_EQ(ma_bare_decision("Sure, go ahead"), 1);
    MARY_ASSERT_EQ(ma_bare_decision("no"), 0);
    MARY_ASSERT_EQ(ma_bare_decision("never mind"), 0);
    MARY_ASSERT_EQ(ma_bare_decision("yes but only the intro"), -1);
    MARY_ASSERT_EQ(ma_bare_decision("play the next song"), -1);
    MARY_ASSERT(ma_is_question_opener("what's") && ma_is_request_frame("could", "we") && !ma_is_request_frame("can", "they"));
}

int main(void) {
    MARY_RUN(replace_splits_target_from_payload_non_greedily);
    MARY_RUN(anaphora_confirmations_and_request_frames_are_peeled);
    MARY_RUN(questions_and_addresses_to_her_are_not_edits);
    MARY_RUN(delete_insert_and_move_have_their_shapes);
    MARY_RUN(the_named_part_is_the_phrase_to_hand_a_read);
    MARY_RUN(question_forms_and_bare_decisions);
    MARY_TEST_MAIN_END();
}
