#include "mary_test.h"
#include "voice/barge_in.h"
#include "voice/state.h"
#include "voice/wake.h"

/* WakePlannerTests.swift and EndpointHoldTests.swift, row for row. */

MARY_TEST(theWakeTable) {
    static const struct { const char *name, *text; mv_wake expected; const char *request; } rows[] = {
        { "the name alone", "Mary", MV_WAKE_BARE, NULL },
        { "greeting prefix", "Hey Mary", MV_WAKE_BARE, NULL },
        { "punctuated", "hey mary?", MV_WAKE_BARE, NULL },
        { "okay-comma form", "Okay, Mary.", MV_WAKE_BARE, NULL },
        { "hi form", "Hi Mary", MV_WAKE_BARE, NULL },
        { "leading recognizer punctuation", "\xE2\x80\x94 Mary", MV_WAKE_BARE, NULL },
        { "request after comma", "Hey Mary, what's the weather?", MV_WAKE_REQUEST, "what's the weather?" },
        { "request unpunctuated", "Mary open my email", MV_WAKE_REQUEST, "open my email" },
        { "request after a dash", "Hey Mary \xE2\x80\x94 turn it down", MV_WAKE_REQUEST, "turn it down" },
        { "preamble stack", "ok hey mary play some jazz", MV_WAKE_REQUEST, "play some jazz" },
        { "embedded word", "rosemary", MV_WAKE_NONE, NULL },
        { "ordinary English", "give me a summary", MV_WAKE_NONE, NULL },
        { "ordinary English", "the primary reason", MV_WAKE_NONE, NULL },
        { "mentioned, not addressed", "tell mary I said hi", MV_WAKE_NONE, NULL },
        { "mid-sentence mention", "I told Mary about it", MV_WAKE_NONE, NULL },
        { "article prefix", "the mary situation", MV_WAKE_NONE, NULL },
        { "empty", "", MV_WAKE_NONE, NULL },
        { "ordinary sentence", "so what should we do about friday", MV_WAKE_NONE, NULL },
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        char request[256];
        mv_wake got = mv_wake_in(rows[i].text, request, sizeof request);
        if (got != rows[i].expected) MARY_FAIL("%s: \"%s\" gave %d", rows[i].name, rows[i].text, (int)got);
        if (rows[i].request && strcmp(request, rows[i].request) != 0) MARY_FAIL("%s: request \"%s\"", rows[i].name, request);
    }
}

MARY_TEST(theEarlyAbortTable) {
    static const struct { const char *name, *partial; bool expected; } rows[] = {
        { "empty partial", "", true },
        { "preamble only", "hey", true },
        { "the name is forming", "hey mar", true },
        { "a greeting word is forming", "he", true },
        { "the name landed", "mary", true },
        { "wake with request underway", "hey mary what", true },
        { "conversation", "so I was saying", false },
        { "another assistant", "hey siri play", false },
        { "ordinary opener mid-word", "hello there we", false },
        { "mention can't lead", "tell mary", false },
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++)
        if (mv_could_still_wake(rows[i].partial) != rows[i].expected) MARY_FAIL("%s: \"%s\"", rows[i].name, rows[i].partial);
}

MARY_TEST(theStopListeningTable) {
    static const struct { const char *name, *text; bool expected; } rows[] = {
        { "bare command", "stop listening", true },
        { "punctuated", "Stop listening.", true },
        { "addressed", "Mary, stop listening", true },
        { "okay prefix", "okay stop listening", true },
        { "polite", "please stop listening", true },
        { "fully dressed", "mary please stop listening now", true },
        { "quit variant", "hey mary quit listening", true },
        { "bare stop is not ours", "stop", false },
        { "addressed bare stop is not ours", "mary stop", false },
        { "stop something else", "stop the music", false },
        { "listening to something", "stop listening to them", false },
        { "opposite", "keep listening", false },
        { "empty", "", false },
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++)
        if (mv_is_stop_listening(rows[i].text) != rows[i].expected) MARY_FAIL("%s: \"%s\"", rows[i].name, rows[i].text);
}

MARY_TEST(danglingEndingsWait) {
    static const char *const partials[] = { "open the", "search for", "play something by", "open Safari and", "um", "go to the,", "open my" };
    for (size_t i = 0; i < sizeof partials / sizeof partials[0]; i++)
        if (mv_endpoint_extra_silence(partials[i]) != MV_DANGLING_EXTENSION) MARY_FAIL("\"%s\" should wait", partials[i]);
}

MARY_TEST(finishedEndingsDoNot) {
    static const char *const partials[] = { "open Safari", "turn it on", "log in", "turn it up", "what time is it", "Hey Mary.", "" };
    for (size_t i = 0; i < sizeof partials / sizeof partials[0]; i++)
        if (mv_endpoint_extra_silence(partials[i]) != 0) MARY_FAIL("\"%s\" should not wait", partials[i]);
}

MARY_TEST(states_are_named_for_the_desktop) {
    MARY_ASSERT_STR(mv_state_name(MV_STATE_HEARING), "hearing");
    MARY_ASSERT_STR(mv_state_name(MV_STATE_SPEAKING), "speaking");
    MARY_ASSERT(mv_state_name(MV_STATE_COUNT) == NULL);
    mv_state s = MV_STATE_IDLE;
    MARY_ASSERT(mv_state_from_name("transcribing", &s) && s == MV_STATE_TRANSCRIBING);
    MARY_ASSERT(!mv_state_from_name("dreaming", &s));
    mv_barge_in governor;
    mv_barge_in_init(&governor, 0.045f, 500);
    MARY_ASSERT(!governor.enabled);
    MARY_ASSERT_EQ(mv_barge_in_process(&governor, 0.9f, 0.02, true), MV_BARGE_NONE);   /* deviation 6 */
}

int main(void) {
    MARY_RUN(theWakeTable);
    MARY_RUN(theEarlyAbortTable);
    MARY_RUN(theStopListeningTable);
    MARY_RUN(danglingEndingsWait);
    MARY_RUN(finishedEndingsDoNot);
    MARY_RUN(states_are_named_for_the_desktop);
    MARY_TEST_MAIN_END();
}
