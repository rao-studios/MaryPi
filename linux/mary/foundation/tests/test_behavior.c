/* BehavioralEpisode and its codec: the bytes are fixed by fixture (sorted keys, ISO-8601 fractional UTC),
 * the first seal wins, and a decode reads back what was written. */
#include "foundation/behavior.h"
#include "mary_test.h"
#include <errno.h>

MARY_TEST(the_codec_writes_sorted_keys_and_utc_dates_with_milliseconds) {
    mf_behavior_episode e;
    mf_behavior_open(&e, "6f1d2a3b-4c5d-4e6f-8a9b-0c1d2e3f4a5b", "play the next song", 1757700000.123, "mistral", "embedding", "0.1.0");
    mf_action_record a = { .disposition = MF_DISPOSITION_SUCCEEDED, .undoable = false, .adapter_count = 1 };
    snprintf(a.id, sizeof a.id, "call-1");
    snprintf(a.intention, sizeof a.intention, "media__next");
    snprintf(a.arguments_json, sizeof a.arguments_json, "{\"b\":1,\"a\":\"x\"}");
    snprintf(a.skill.package_id, sizeof a.skill.package_id, "media");
    snprintf(a.skill.ability_id, sizeof a.skill.ability_id, "multimedia");
    snprintf(a.skill.skill_id, sizeof a.skill.skill_id, "next");
    snprintf(a.skill.invocation, sizeof a.skill.invocation, "media__next");
    snprintf(a.adapters[0], 32, "desktop");
    snprintf(a.summary, sizeof a.summary, "Skipped to the next track.");
    MARY_ASSERT_EQ(mf_behavior_append(&e, &a), 0);
    mf_behavior_add_target(&e, "multimedia", "discipline");
    mf_behavior_add_target(&e, "media", "applicationExpertise");
    mf_behavior_add_target(&e, "multimedia", "discipline");        /* unique */
    MARY_ASSERT_EQ(e.target_count, 2);
    MARY_ASSERT_STR(e.targets[0].ability_id, "media");              /* sorted */
    mf_behavior_seal(&e, MF_SEAL_COMPLETED, 1757700001.5);
    mf_behavior_seal(&e, MF_SEAL_CANCELLED, 1757700002);           /* the first reason wins */
    MARY_ASSERT_EQ(e.sealed_reason, MF_SEAL_COMPLETED);
    MARY_ASSERT(mf_behavior_did_act(&e));
    char *json = mf_behavior_encode(&e);
    static const char EXPECTED[] =
        "{\"abilityTargets\":[{\"abilityID\":\"media\",\"paradigm\":\"applicationExpertise\"},{\"abilityID\":\"multimedia\",\"paradigm\":\"discipline\"}],"
        "\"id\":\"6f1d2a3b-4c5d-4e6f-8a9b-0c1d2e3f4a5b\",\"input\":{\"query\":\"play the next song\"},\"openedAt\":\"2025-09-12T18:00:00.123Z\","
        "\"output\":{\"actions\":[{\"action\":{\"adapters\":[\"desktop\"],\"argumentsJSON\":\"{\\\"b\\\":1,\\\"a\\\":\\\"x\\\"}\",\"intention\":\"media__next\","
        "\"skill\":{\"abilityID\":\"multimedia\",\"digest\":\"\",\"invocationName\":\"media__next\",\"packageID\":\"media\",\"skillID\":\"next\",\"version\":\"1.0.0\"}},"
        "\"disposition\":\"succeeded\",\"foundNothing\":false,\"id\":\"call-1\",\"initiator\":\"model\",\"summary\":\"Skipped to the next track.\",\"undoable\":false}]},"
        "\"provenance\":{\"appVersion\":\"0.1.0\",\"engine\":\"mistral\",\"lane\":\"embedding\"},\"schema\":\"mary.behavior\",\"schemaVersion\":1,"
        "\"sealedAt\":\"2025-09-12T18:00:01.500Z\",\"sealedReason\":\"completed\"}";
    MARY_ASSERT_STR(json, EXPECTED);
    mf_behavior_episode back;
    MARY_ASSERT_EQ(mf_behavior_decode(json, strlen(json), &back), 0);
    MARY_ASSERT_STR(back.id, e.id);
    MARY_ASSERT_NEAR(back.opened_at, 1757700000.123, 1e-3);
    MARY_ASSERT_EQ(back.sealed_reason, MF_SEAL_COMPLETED);
    MARY_ASSERT_EQ(back.action_count, 1);
    MARY_ASSERT_STR(back.actions[0].skill.invocation, "media__next");
    MARY_ASSERT_EQ(back.target_count, 2);
    char *again = mf_behavior_encode(&back);
    MARY_ASSERT_STR(again, json);                                  /* the codec is stable */
    free(again);
    free(json);
    MARY_ASSERT_EQ(mf_behavior_decode("{\"schema\":\"other\"}", 17, &back), -EBADMSG);
    mf_behavior_free(&e);
    mf_behavior_free(&back);
}

MARY_TEST(arguments_are_canonical_and_ids_are_uuids) {
    char *c = mf_canonical_json("  {\"z\": {\"b\": 2, \"a\": [3, {\"y\": 1, \"x\": 0}]}, \"a\": 1}  ");
    MARY_ASSERT_STR(c, "{\"a\":1,\"z\":{\"a\":[3,{\"x\":0,\"y\":1}],\"b\":2}}");
    free(c);
    c = mf_canonical_json(" not json ");
    MARY_ASSERT_STR(c, "not json");
    free(c);
    char id[MF_UUID_LEN + 1], other[MF_UUID_LEN + 1];
    mf_uuid_v4(id);
    mf_uuid_v4(other);
    MARY_ASSERT_EQ(strlen(id), 36);
    MARY_ASSERT(id[8] == '-' && id[13] == '-' && id[14] == '4' && id[18] == '-' && (id[19] == '8' || id[19] == '9' || id[19] == 'a' || id[19] == 'b') && id[23] == '-');
    MARY_ASSERT(strcmp(id, other) != 0);
    char when[40];
    mf_iso8601(0, when, sizeof when);
    MARY_ASSERT_STR(when, "1970-01-01T00:00:00.000Z");
    double back = 0;
    MARY_ASSERT(mf_iso8601_parse("2026-09-13T10:00:00.5Z", &back));
    mf_iso8601(back, when, sizeof when);
    MARY_ASSERT_STR(when, "2026-09-13T10:00:00.500Z");       /* the parse is the format's inverse */
    MARY_ASSERT(mf_iso8601_parse("1970-01-01T00:00:00.000Z", &back) && back == 0);
    MARY_ASSERT(!mf_iso8601_parse("2026-09-13", &back));
}

int main(void) {
    MARY_RUN(the_codec_writes_sorted_keys_and_utc_dates_with_milliseconds);
    MARY_RUN(arguments_are_canonical_and_ids_are_uuids);
    MARY_TEST_MAIN_END();
}
