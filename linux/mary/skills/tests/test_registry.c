#include <errno.h>
#include <ctype.h>

#include "common/json.h"
#include "mary_test.h"
#include "skills/registry.h"

/* The three example skills the desktop declares first, under three policies. */
static const char *MESSAGE =
    "{\"type\":\"skills\",\"apps\":["
    "{\"id\":\"settings\",\"name\":\"System Settings\",\"enabled\":true,\"ask\":\"never\",\"skills\":["
    "{\"id\":\"open_pane\",\"title\":\"Open a pane\",\"summary\":\"Shows one pane of System Settings.\","
    "\"params\":{\"type\":\"object\",\"properties\":{\"pane\":{\"type\":\"string\"}},\"required\":[\"pane\"]},\"effect\":\"act\",\"enabled\":true}]},"
    "{\"id\":\"media\",\"name\":\"Media Player\",\"enabled\":false,\"ask\":\"changes\",\"skills\":["
    "{\"id\":\"play_pause\",\"title\":\"Play or pause\",\"summary\":\"Toggles playback.\",\"params\":null,\"effect\":\"act\",\"enabled\":true}]},"
    "{\"id\":\"calendar\",\"name\":\"Calendar\",\"enabled\":true,\"ask\":\"changes\",\"skills\":["
    "{\"id\":\"events_today\",\"title\":\"Today's events\",\"summary\":\"Lists what is on today.\",\"params\":null,\"effect\":\"read\",\"enabled\":true},"
    "{\"id\":\"new_event\",\"title\":\"New event\",\"summary\":\"Adds an event.\",\"params\":null,\"effect\":\"act\",\"enabled\":true},"
    "{\"id\":\"delete_event\",\"title\":\"Delete an event\",\"summary\":\"Removes an event.\",\"params\":null,\"effect\":\"destructive\",\"enabled\":true},"
    "{\"id\":\"events.week\",\"title\":\"This week\",\"summary\":\"Lists the week.\",\"params\":null,\"effect\":\"read\",\"enabled\":false}]}]}";

static void load(sk_registry *r, const char *text) {
    struct json_object *m = mc_json_parse(text, strlen(text));
    sk_registry_init(r);
    MARY_ASSERT_EQ(sk_registry_load(r, m), 0);
    json_object_put(m);
}

MARY_TEST(the_desktops_message_becomes_the_registry) {
    sk_registry r;
    load(&r, MESSAGE);
    MARY_ASSERT_EQ(r.app_count, 3);
    const sk_app *calendar = sk_registry_app(&r, "calendar");
    MARY_ASSERT(calendar && calendar->skill_count == 4 && calendar->ask == SK_ASK_CHANGES);
    const sk_skill *open_pane = sk_registry_skill(&r, "settings", "open_pane");
    MARY_ASSERT(open_pane && open_pane->params && open_pane->effect == SK_EFFECT_ACT);
    MARY_ASSERT(sk_registry_skill(&r, "calendar", "open_pane") == NULL);
    struct json_object *bad = mc_json_parse("{\"apps\":[{\"id\":\"x\",\"skills\":[{\"id\":\"y\",\"effect\":\"explode\"}]}]}", 59);
    MARY_ASSERT_EQ(sk_registry_load(&r, bad), -EINVAL);
    MARY_ASSERT_EQ(r.app_count, 3);   /* a bad message keeps what was there */
    json_object_put(bad);
    sk_registry_free(&r);
}

MARY_TEST(decisions_follow_the_desktops_rule) {
    sk_registry r;
    load(&r, MESSAGE);
    static const struct { const char *app, *skill; sk_decision expected; } rows[] = {
        { "settings", "open_pane", SK_ALLOWED },                 /* acts, but Settings never asks */
        { "media", "play_pause", SK_DENIED },                    /* the app is turned off */
        { "calendar", "events_today", SK_ALLOWED },              /* reads never need asking under "changes" */
        { "calendar", "new_event", SK_NEEDS_CONFIRMATION },      /* acts under "changes" */
        { "calendar", "delete_event", SK_NEEDS_CONFIRMATION },   /* destructive is always confirmed */
        { "calendar", "events.week", SK_DENIED },                /* the skill is turned off */
        { "calendar", "nope", SK_UNKNOWN },
        { "terminal", "run", SK_UNKNOWN },
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        sk_decision got = sk_registry_decide(&r, rows[i].app, rows[i].skill);
        if (got != rows[i].expected) MARY_FAIL("%s.%s: %s", rows[i].app, rows[i].skill, sk_decision_name(got));
    }
    MARY_ASSERT_STR(sk_decision_name(SK_NEEDS_CONFIRMATION), "needs_confirmation");
    MARY_ASSERT_STR(sk_effect_name(SK_EFFECT_DESTRUCTIVE), "destructive");
    MARY_ASSERT_STR(sk_ask_name(SK_ASK_ALWAYS), "always");
    sk_registry_free(&r);
}

MARY_TEST(only_allowed_skills_become_mistral_tools) {
    sk_registry r;
    load(&r, MESSAGE);
    struct json_object *tools = sk_tools_json(&r);
    MARY_ASSERT_EQ(json_object_array_length(tools), 4);     /* the two that need confirmation ride too: the lane parks them */
    struct json_object *first = mc_json_object(json_object_array_get_idx(tools, 0), "function");
    MARY_ASSERT_STR(mc_json_type(json_object_array_get_idx(tools, 0)), "function");
    MARY_ASSERT_STR(mc_json_string(first, "name"), "settings__open_pane");
    MARY_ASSERT_STR(mc_json_string(first, "description"), "Open a pane (System Settings): Shows one pane of System Settings.");
    MARY_ASSERT(mc_json_object(mc_json_object(first, "parameters"), "properties") != NULL);
    struct json_object *second = mc_json_object(json_object_array_get_idx(tools, 1), "function");
    MARY_ASSERT_STR(mc_json_string(second, "name"), "calendar__events_today");
    MARY_ASSERT_STR(mc_json_string(mc_json_object(second, "parameters"), "type"), "object");
    for (size_t i = 0; i < json_object_array_length(tools); i++) {
        const char *name = mc_json_string(mc_json_object(json_object_array_get_idx(tools, i), "function"), "name");
        for (const char *p = name; *p; p++) if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') MARY_FAIL("%s", name);
    }
    json_object_put(tools);
    const sk_app *app;
    const sk_skill *skill;
    MARY_ASSERT(sk_tool_lookup(&r, "calendar__events_week", &app, &skill));   /* the dot became '_' */
    MARY_ASSERT_STR(skill->id, "events.week");
    MARY_ASSERT(!sk_tool_lookup(&r, "calendar__nope", NULL, NULL));
    sk_registry_free(&r);
}

MARY_TEST(the_richer_schema_defaults_from_the_effect_and_a_style_record_goes_to_the_thread) {
    static const char TEXT[] =
        "{\"type\":\"skills\",\"apps\":[{\"id\":\"media\",\"name\":\"Media Player\",\"enabled\":true,\"ask\":\"never\",\"summary\":\"Plays music and video.\","
        "\"aliases\":[\"player\"],\"discipline\":\"multimedia\",\"perception\":[\"file\",\"playing\"],\"skills\":["
        "{\"id\":\"play_pause\",\"title\":\"Play or pause\",\"summary\":\"Plays or pauses.\",\"params\":null,\"effect\":\"act\",\"enabled\":true,"
        "\"triggers\":{\"tokens\":[\"play\",\"pause\"],\"phrases\":[\"play the music\"]},\"target_classes\":[\"media\"]},"
        "{\"id\":\"eject\",\"title\":\"Eject\",\"summary\":\"Ejects the disc.\",\"params\":null,\"effect\":\"destructive\",\"enabled\":true,\"kind\":\"workflow\"}]}]}";
    struct json_object *msg = mc_json_parse(TEXT, strlen(TEXT));
    sk_registry r;
    sk_registry_init(&r);
    MARY_ASSERT_EQ(sk_registry_load(&r, msg), 0);
    json_object_put(msg);
    const sk_app *media = sk_registry_app(&r, "media");
    MARY_ASSERT_STR(media->title, "Media Player");
    MARY_ASSERT_STR(media->paradigm, "applicationExpertise");
    MARY_ASSERT_STR(media->discipline, "multimedia");
    MARY_ASSERT_EQ(media->alias_count, 1);
    MARY_ASSERT_EQ(media->perception_count, 2);
    const sk_skill *play = sk_registry_skill(&r, "media", "play_pause"), *eject = sk_registry_skill(&r, "media", "eject");
    MARY_ASSERT_EQ(play->kind, SK_KIND_EFFECTFUL);
    MARY_ASSERT_EQ(play->access, SK_ACCESS_REVERSIBLE);
    MARY_ASSERT_EQ(play->trigger_count, 2);
    MARY_ASSERT_STR(play->phrases[0], "play the music");
    MARY_ASSERT_STR(play->invocation, "media__play_pause");
    MARY_ASSERT_EQ(eject->kind, SK_KIND_WORKFLOW);
    MARY_ASSERT_EQ(eject->access, SK_ACCESS_CONFIRM);
    /* a skill that needs confirmation is still a tool: the lane parks it for the person */
    struct json_object *tools = sk_tools_json(&r);
    MARY_ASSERT_EQ(json_object_array_length(tools), 2);
    json_object_put(tools);
    char group[80];
    sk_ability_group("mary", "media", "applicationExpertise", group, sizeof group);
    MARY_ASSERT(strncmp(group, "mary-ability-", 13) == 0 && strlen(group) == 13 + 16);
    char again[80];
    sk_ability_group("Mary ", "MEDIA", "applicationExpertise", again, sizeof again);
    MARY_ASSERT_STR(again, group);                              /* canonical: lowercase, whitespace folded */
    /* the Thread gets one style record per discipline, and nothing per skill or app */
    struct json_object *records = sk_style_records(&r, "mary");
    MARY_ASSERT_EQ(json_object_array_length(records), 1);
    struct json_object *style = json_object_array_get_idx(records, 0);
    MARY_ASSERT(strncmp(mc_json_string(style, "document_id"), "mary-style-profile-", 19) == 0 && strlen(mc_json_string(style, "document_id")) == 19 + 16);
    MARY_ASSERT_STR(mc_json_string(style, "group"), "mary-style-mary");
    MARY_ASSERT_STR(mc_json_string(style, "label"), "Style");
    MARY_ASSERT_STR(mc_json_string(style, "family"), "style");
    MARY_ASSERT_STR(mc_json_string(style, "name"), "Multimedia");
    const char *text = json_object_get_string(json_object_array_get_idx(mc_json_array(style, "texts"), 0));
    MARY_ASSERT(strstr(text, "Discipline: Multimedia\nRealized by:\n- Media Player: Play or pause, Eject\n") != NULL);
    MARY_ASSERT(strstr(text, "Listens for: play, pause\n") && strstr(text, "Phrases: play the music\n"));
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(style, "relationships")), 1);   /* Media Player practices multimedia */
    MARY_ASSERT_STR(mc_json_string(mc_json_object(style, "metadata"), "discipline"), "multimedia");
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(mc_json_object(style, "metadata"), "apps")), 1);
    struct json_object *same = sk_style_records(&r, "mary");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(same, 0), "document_id"), mc_json_string(style, "document_id"));   /* idempotent */
    json_object_put(same);
    json_object_put(records);
    sk_registry_free(&r);
}

int main(void) {
    MARY_RUN(the_desktops_message_becomes_the_registry);
    MARY_RUN(decisions_follow_the_desktops_rule);
    MARY_RUN(only_allowed_skills_become_mistral_tools);
    MARY_RUN(the_richer_schema_defaults_from_the_effect_and_a_style_record_goes_to_the_thread);
    MARY_TEST_MAIN_END();
}
