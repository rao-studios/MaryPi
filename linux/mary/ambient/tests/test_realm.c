/* AmbientRealmResolverTests and the focus signal: need → candidates sorted by token → the place ladder. */
#include "ambient/realm.h"
#include "mary_test.h"

static ma_roster roster;

MARY_TEST(the_focus_ledger_keeps_one_stamp_per_place_and_projects_a_signal) {
    ma_focus_ledger ledger;
    ma_focus_init(&ledger);
    ma_place textedit = ma_place_application("textedit"), finder = ma_place_application("finder"), media = ma_place_application("media");
    ma_focus_note(&ledger, &finder, MA_EVIDENCE_ACTIVATION, 100);
    ma_focus_note(&ledger, &textedit, MA_EVIDENCE_ACTIVATION, 110);
    ma_focus_note(&ledger, &media, MA_EVIDENCE_GLANCE, 120);
    ma_focus_note(&ledger, &finder, MA_EVIDENCE_ACTIVITY, 130);
    MARY_ASSERT_EQ(ledger.count, 3);
    ma_focus_signal s;
    ma_focus_project(&ledger, 131, &s);
    MARY_ASSERT(s.has_lead && ma_place_equal(&s.lead, &finder));     /* activity takes the lead */
    MARY_ASSERT_EQ(s.co_active_count, 2);
    MARY_ASSERT_STR(s.co_active[0].application, "textedit");         /* activation outranks a glance */
    MARY_ASSERT_STR(s.co_active[1].application, "media");
    MARY_ASSERT_EQ(s.glanced_count, 1);
    ma_focus_project(&ledger, 130 + 301, &s);                        /* 5 min on: activation faded, the glance (10 min) stays */
    MARY_ASSERT_EQ(s.co_active_count, 1);
    MARY_ASSERT_STR(s.co_active[0].application, "media");
    MARY_ASSERT(s.has_lead);
    ma_focus_project(&ledger, 130 + 1201, &s);
    MARY_ASSERT(!s.has_lead && s.co_active_count == 0);
}

MARY_TEST(an_empty_need_admits_everyone_with_eyes) {
    ma_realm_inputs in = { .utterance = "what's that?", .roster = &roster, .now = 100 };
    ma_realm realm;
    ma_realm_resolve(&in, &realm);
    MARY_ASSERT(ma_need_is_empty(&realm.need));
    MARY_ASSERT_EQ(realm.candidate_count, 10);       /* every seeded app with eyes */
    for (int i = 1; i < realm.candidate_count; i++) MARY_ASSERT(ma_place_compare(&realm.candidates[i - 1].place, &realm.candidates[i].place) < 0);
    MARY_ASSERT(!realm.has_place && realm.decided_by == MA_SIGNAL_NONE);
}

MARY_TEST(with_nothing_open_the_closest_application_of_the_need_is_the_spawn) {
    /* a writing need, nobody in focus: TextEdit is the one an action turn would open */
    ma_realm_inputs in = { .utterance = "can you write hello world in a new note", .roster = &roster, .now = 100 };
    ma_realm realm;
    ma_realm_resolve(&in, &realm);
    MARY_ASSERT(!ma_need_is_empty(&realm.need));
    MARY_ASSERT(!realm.has_place);
    MARY_ASSERT(realm.has_spawn);
    MARY_ASSERT_STR(realm.spawn.application, "textedit");
    /* the discipline alone names it too */
    ma_realm_inputs cue = { .utterance = "tighten it", .discipline = "writing", .roster = &roster, .now = 100 };
    ma_realm_resolve(&cue, &realm);
    MARY_ASSERT(realm.has_spawn && strcmp(realm.spawn.application, "textedit") == 0);
    /* a named place that is not in front is the spawn too */
    ma_place named[1] = { ma_place_application("textedit") };
    ma_realm_inputs addressed = { .utterance = "write hello in textedit", .named = named, .named_count = 1, .roster = &roster, .now = 100 };
    ma_realm_resolve(&addressed, &realm);
    MARY_ASSERT(realm.has_place && realm.has_spawn && strcmp(realm.spawn.application, "textedit") == 0);
    /* an open, conforming place still wins and no spawn is named */
    ma_focus_signal focus = { .has_lead = true, .lead = ma_place_application("textedit") };
    ma_focus_evidence evidence[] = { { ma_place_application("textedit"), MA_EVIDENCE_ACTIVATION, 90 } };
    in.focus = &focus;
    in.evidence = evidence;
    in.evidence_count = 1;
    in.decided_by = MA_SIGNAL_ACTION_COMMAND;
    ma_realm_resolve(&in, &realm);
    MARY_ASSERT(realm.has_place && !realm.has_spawn);
    /* nothing asked for: nothing to open */
    ma_realm_inputs chat = { .utterance = "how was your day", .roster = &roster, .now = 100 };
    ma_realm_resolve(&chat, &realm);
    MARY_ASSERT(!realm.has_spawn);
}

MARY_TEST(a_need_narrows_the_realm_and_focus_decides_the_place) {
    ma_focus_signal focus = { .has_lead = true, .lead = ma_place_application("textedit") };
    ma_focus_evidence evidence[] = { { ma_place_application("textedit"), MA_EVIDENCE_ACTIVATION, 90 } };
    ma_realm_inputs in = { .utterance = "tighten the intro", .discipline = "writing", .decided_by = MA_SIGNAL_EDIT_INTENT, .focus = &focus,
                           .evidence = evidence, .evidence_count = 1, .roster = &roster, .now = 100 };
    ma_realm realm;
    ma_realm_resolve(&in, &realm);
    MARY_ASSERT_EQ(realm.need.ability_count, 1);
    MARY_ASSERT_STR(realm.need.abilities[0], "writing");
    MARY_ASSERT_EQ(realm.candidate_count, 1);
    MARY_ASSERT(realm.candidates[0].conforms_by_discipline && realm.candidates[0].conforms_count == 1 && realm.candidates[0].has_evidence);
    MARY_ASSERT_NEAR(realm.candidates[0].evidence_age, 10.0, 1e-9);
    MARY_ASSERT(realm.has_place && ma_place_equal(&realm.place, &focus.lead));
    MARY_ASSERT_EQ(realm.decided_by, MA_SIGNAL_EDIT_INTENT);
    MARY_ASSERT(ma_realm_chosen(&realm) == &realm.candidates[0]);
    /* the lead does not conform: the strongest conforming co-active place takes it */
    focus.lead = ma_place_application("finder");
    focus.co_active[0] = ma_place_application("media");
    focus.co_active[1] = ma_place_application("textedit");
    focus.co_active_count = 2;
    ma_realm_resolve(&in, &realm);
    MARY_ASSERT(realm.has_place);
    MARY_ASSERT_STR(realm.place.application, "textedit");
    /* nobody is anywhere that conforms: no place, and that is a real answer */
    focus.co_active_count = 0;
    ma_realm_resolve(&in, &realm);
    MARY_ASSERT(!realm.has_place && realm.decided_by == MA_SIGNAL_NONE);
}

MARY_TEST(a_named_place_outranks_focus_and_conformance) {
    ma_place named[] = { ma_place_application("media"), ma_place_application("calendar") };
    ma_focus_signal focus = { .has_lead = true, .lead = ma_place_application("textedit") };
    ma_realm_inputs in = { .utterance = "play it", .named = named, .named_count = 2, .decided_by = MA_SIGNAL_ACTION_COMMAND, .focus = &focus, .roster = &roster, .now = 100 };
    ma_realm realm;
    ma_realm_resolve(&in, &realm);
    MARY_ASSERT_EQ(realm.need.ability_count, 1);
    MARY_ASSERT_STR(realm.need.abilities[0], "multimedia");
    MARY_ASSERT(realm.has_place);
    MARY_ASSERT_STR(realm.place.application, "media");     /* the conforming named place, whichever was named first */
    /* a named place nobody conforms to is still the answer: names are addresses */
    ma_place only[] = { ma_place_application("calendar") };
    in.named = only;
    in.named_count = 1;
    ma_realm_resolve(&in, &realm);
    MARY_ASSERT(realm.has_place);
    MARY_ASSERT_STR(realm.place.application, "calendar");
}

int main(void) {
    ma_roster_maryos(&roster);
    MARY_RUN(the_focus_ledger_keeps_one_stamp_per_place_and_projects_a_signal);
    MARY_RUN(an_empty_need_admits_everyone_with_eyes);
    MARY_RUN(with_nothing_open_the_closest_application_of_the_need_is_the_spawn);
    MARY_RUN(a_need_narrows_the_realm_and_focus_decides_the_place);
    MARY_RUN(a_named_place_outranks_focus_and_conformance);
    MARY_TEST_MAIN_END();
}
