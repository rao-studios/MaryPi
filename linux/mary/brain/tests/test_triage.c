/* TurnTriage and EmbeddingRouting: the index over a scripted embedder, the floor and the margin,
 * the argument shapes, the spoken extractors, the single-clause rule and the deterministic tier. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "brain/scope.h"
#include "brain/triage.h"
#include "common/json.h"
#include "mary_test.h"

static sk_registry registry;

static const char REGISTRY[] =
    "{\"type\":\"skills\",\"apps\":["
    "{\"id\":\"media\",\"name\":\"Media Player\",\"enabled\":true,\"ask\":\"never\",\"aliases\":[\"player\"],\"skills\":["
    "{\"id\":\"play_pause\",\"title\":\"Play or pause\",\"summary\":\"Plays or pauses.\",\"params\":null,\"effect\":\"act\",\"enabled\":true,\"triggers\":{\"tokens\":[\"play\",\"pause\"],\"phrases\":[\"play the music\"]}},"
    "{\"id\":\"transport\",\"title\":\"Skip\",\"summary\":\"Skips forward or back.\",\"params\":{\"type\":\"object\",\"properties\":{\"direction\":{\"type\":\"string\",\"enum\":[\"next\",\"previous\"]}},\"required\":[\"direction\"]},"
    "\"effect\":\"act\",\"enabled\":true,\"spoken\":{\"direction\":{\"next\":[\"skip\",\"next song\"],\"previous\":[\"go back\",\"last song\"]}}}]},"
    "{\"id\":\"textedit\",\"name\":\"TextEdit\",\"enabled\":true,\"ask\":\"never\",\"skills\":["
    "{\"id\":\"open\",\"title\":\"Open a document\",\"summary\":\"Opens a document by name.\",\"params\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]},"
    "\"effect\":\"read\",\"enabled\":true,\"triggers\":{\"tokens\":[\"open\"],\"phrases\":[\"open the document\"]}},"
    "{\"id\":\"write\",\"title\":\"Write\",\"summary\":\"Writes prose.\",\"params\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"x-composition\":true}},\"required\":[\"text\"]},\"effect\":\"act\",\"enabled\":true},"
    "{\"id\":\"new_document\",\"title\":\"Write a new note\",\"summary\":\"Opens a fresh note and types the text into it.\",\"params\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]},"
    "\"effect\":\"act\",\"enabled\":true,\"triggers\":{\"tokens\":[\"write\",\"note\",\"new\",\"document\"],\"phrases\":[\"write a new note\",\"new note\"]}}]},"
    "{\"id\":\"settings\",\"name\":\"System Settings\",\"enabled\":false,\"ask\":\"never\",\"skills\":[{\"id\":\"open_pane\",\"title\":\"Open a pane\",\"summary\":\"\",\"params\":null,\"effect\":\"read\",\"enabled\":true}]}]}";

/* A scripted embedder: a text's vector is its bag of a few known words, so cosines are predictable. */
static const char *const WORDS[] = { "play", "pause", "music", "skip", "song", "open", "document", "write", "prose", "pane", "settings", NULL };
static int fake_embed(const char *const *texts, size_t n, float **out, size_t *dim, char *message, size_t cap, void *user) {
    size_t d = 11;
    float *v = calloc(n * d, sizeof *v);
    for (size_t i = 0; i < n; i++) {
        char lower[4096];
        snprintf(lower, sizeof lower, "%s", texts[i]);
        for (char *c = lower; *c; c++) *c = *c >= 'A' && *c <= 'Z' ? (char)(*c + 32) : *c;
        for (size_t w = 0; w < d; w++) { const char *at = strstr(lower, WORDS[w]); v[i * d + w] = at ? 1.0f : 0.0f; }
    }
    *out = v;
    *dim = d;
    return 0;
}

MARY_TEST(the_index_embeds_every_skill_once_and_a_turn_finds_its_winner) {
    mb_skill_index index = { 0 };
    char message[120] = "";
    MARY_ASSERT_EQ(mb_skill_index_build(&index, &registry, fake_embed, NULL, message, sizeof message), 0);
    MARY_ASSERT_EQ(index.n, 6);
    MARY_ASSERT_EQ(index.dim, 11);
    MARY_ASSERT_STR(index.items[0].invocation, "media__play_pause");
    char text[2048];
    mb_skill_index_text(&registry.apps[0], &registry.apps[0].skills[0], text, sizeof text);
    MARY_ASSERT_STR(text, "Play or pause. Plays or pauses. play the music. play, pause.");
    float *q = NULL;
    size_t dim = 0;
    const char *utterance[] = { "play the music" };
    fake_embed(utterance, 1, &q, &dim, message, sizeof message, NULL);
    mb_affinity a[8];
    int n = mb_affinities(&index, q, a, 8);
    MARY_ASSERT_EQ(n, 6);
    MARY_ASSERT_STR(a[0].skill->invocation, "media__play_pause");
    MARY_ASSERT(a[0].score > 0.8f);               /* play, music of play, pause, music */
    const mb_skill_vector *winner = mb_unique_winner(a, n, &registry, MB_ROUTING_FLOOR, MB_ROUTING_MARGIN);
    MARY_ASSERT(winner && strcmp(winner->invocation, "media__play_pause") == 0);
    /* nothing above the floor: no winner; two too close: no winner */
    MARY_ASSERT(mb_unique_winner(a, n, &registry, 0.99f, 0.04f) == NULL);
    mb_affinity tie[2] = { { &index.items[0], 0.8f }, { &index.items[1], 0.78f } };
    MARY_ASSERT(mb_unique_winner(tie, 2, &registry, 0.62f, 0.04f) == NULL);
    /* a skill the desktop turned off cannot win */
    mb_affinity off[1] = { { &index.items[5], 0.9f } };   /* System Settings, turned off */
    MARY_ASSERT(mb_unique_winner(off, 1, &registry, 0.62f, 0.04f) == NULL);
    free(q);
    mb_skill_index_free(&index);
}

MARY_TEST(the_shape_admits_no_arguments_one_string_or_one_spoken_enum) {
    const sk_skill *play = sk_registry_skill(&registry, "media", "play_pause"), *skip = sk_registry_skill(&registry, "media", "transport"),
                   *open = sk_registry_skill(&registry, "textedit", "open"), *write = sk_registry_skill(&registry, "textedit", "write");
    MARY_ASSERT_EQ(mb_confidence_shape_of(play, "play the music"), MB_SHAPE_NO_REQUIRED_ARGUMENTS);
    MARY_ASSERT_EQ(mb_confidence_shape_of(open, "open the essay"), MB_SHAPE_SINGLE_STRING);
    MARY_ASSERT_EQ(mb_confidence_shape_of(write, "write a poem"), MB_SHAPE_NONE);          /* composition needs a model */
    MARY_ASSERT_EQ(mb_confidence_shape_of(skip, "skip this one"), MB_SHAPE_SINGLE_ENUM);
    MARY_ASSERT_EQ(mb_confidence_shape_of(skip, "pause it and then skip"), MB_SHAPE_SINGLE_ENUM);
    MARY_ASSERT_EQ(mb_confidence_shape_of(skip, "go back and then skip to the next song"), MB_SHAPE_NONE);   /* two values: the model's */
    MARY_ASSERT_EQ(mb_confidence_shape_of(skip, "louder please"), MB_SHAPE_NONE);
    char value[64], spoken[64];
    MARY_ASSERT(mb_spoken_enum(skip, "direction", "Can you go back to the last song?", value, sizeof value, spoken, sizeof spoken));
    MARY_ASSERT_STR(value, "previous");
    MARY_ASSERT_STR(spoken, "last song");         /* the longest phrase first; "go back" reaches the same value */
    MARY_ASSERT(mb_spoken_enum(skip, "direction", "next", value, sizeof value, spoken, sizeof spoken));
    MARY_ASSERT_STR(value, "next");
    MARY_ASSERT(!mb_spoken_enum(skip, "direction", "turn it up", value, sizeof value, spoken, sizeof spoken));
}

MARY_TEST(the_spoken_span_peels_the_preamble_the_app_and_the_trigger) {
    const sk_skill *open = sk_registry_skill(&registry, "textedit", "open");
    const sk_app *textedit = sk_registry_app(&registry, "textedit");
    char span[512], stages[600];
    mb_spoken_span("Hey Mary, can you open the document Tides in TextEdit", open, textedit, span, sizeof span, stages, sizeof stages);
    MARY_ASSERT_STR(span, "Tides");
    MARY_ASSERT(strstr(stages, "preamble: \"open the document Tides in TextEdit\"") != NULL);
    MARY_ASSERT(strstr(stages, "trigger: \"Tides in TextEdit\"") != NULL);
    MARY_ASSERT(strstr(stages, "trailing app: \"Tides\"") != NULL);
    mb_spoken_span("open textedit and open Notes", open, textedit, span, sizeof span, stages, sizeof stages);
    MARY_ASSERT_STR(span, "Notes");
    MARY_ASSERT(strstr(stages, "open clause") != NULL);
    mb_spoken_span("open", open, textedit, span, sizeof span, NULL, 0);
    MARY_ASSERT_STR(span, "open");                 /* never empty: the least-stripped stage stands */
    struct json_object *args = mb_confidence_arguments(open, textedit, "open the document Tides", stages, sizeof stages);
    MARY_ASSERT_STR(mc_json_string(args, "name"), "Tides");
    json_object_put(args);
    const sk_skill *skip = sk_registry_skill(&registry, "media", "transport");
    args = mb_confidence_arguments(skip, sk_registry_app(&registry, "media"), "skip to the next song please", stages, sizeof stages);
    MARY_ASSERT_STR(mc_json_string(args, "direction"), "next");
    MARY_ASSERT(strstr(stages, "enum direction: \"next song\" -> next") != NULL);
    json_object_put(args);
}

MARY_TEST(the_span_drops_the_trailing_place_words_and_a_request_is_action_shaped_by_its_verb) {
    struct json_object *msg = mc_json_parse(REGISTRY, strlen(REGISTRY));
    sk_registry r;
    sk_registry_init(&r);
    sk_registry_load(&r, msg);
    json_object_put(msg);
    const sk_skill *note = sk_registry_skill(&r, "textedit", "new_document");
    const sk_app *textedit = sk_registry_app(&r, "textedit");
    char span[256], stages[600];
    mb_spoken_span("can you write hello world in a new note", note, textedit, span, sizeof span, stages, sizeof stages);
    MARY_ASSERT_STR(span, "hello world");                        /* the preamble, the verb, then "in a new note" peeled */
    MARY_ASSERT(strstr(stages, "trailing words") != NULL);
    mb_spoken_span("write hello world", note, textedit, span, sizeof span, NULL, 0);
    MARY_ASSERT_STR(span, "hello world");
    struct json_object *args = mb_confidence_arguments(note, textedit, "can you write hello world as a note", NULL, 0);
    MARY_ASSERT_STR(mc_json_string(args, "text"), "hello world");
    json_object_put(args);
    /* action-shaped: the first content word is some skill's trigger, or the words land near a skill */
    MARY_ASSERT(mb_action_shaped(NULL, 0, "can you write hello world in a new note", &r));
    MARY_ASSERT(mb_action_shaped(NULL, 0, "Hey Mary, play something", &r));
    MARY_ASSERT(!mb_action_shaped(NULL, 0, "what is the capital of France", &r));
    MARY_ASSERT(!mb_action_shaped(NULL, 0, "how was your day", &r));
    mb_skill_vector v = { .app = "media", .skill = "play_pause" };
    mb_affinity near[1] = { { &v, 0.55f } }, far[1] = { { &v, 0.2f } };
    MARY_ASSERT(mb_action_shaped(near, 1, "how was your day", &r));
    MARY_ASSERT(!mb_action_shaped(far, 1, "how was your day", &r));
    sk_registry_free(&r);
}

MARY_TEST(a_zero_argument_verb_needs_a_whole_simple_sentence_and_bare_answers_are_exact) {
    MARY_ASSERT(mb_is_single_clause("bring all my windows forward"));
    MARY_ASSERT(mb_is_single_clause("Can you go back?"));
    MARY_ASSERT(!mb_is_single_clause("bring them forward and close the last one"));
    MARY_ASSERT(!mb_is_single_clause("pause, then skip"));
    MARY_ASSERT(!mb_is_single_clause("what is this? and that"));
    MARY_ASSERT(!mb_is_single_clause("one two three four five six seven eight nine ten eleven twelve thirteen"));
    MARY_ASSERT(!mb_is_single_clause("   "));
    MARY_ASSERT_EQ(mb_deterministic_decision("Yes, go ahead."), 1);
    MARY_ASSERT_EQ(mb_deterministic_decision("okay do it"), 1);
    MARY_ASSERT_EQ(mb_deterministic_decision("Never mind"), 0);
    MARY_ASSERT_EQ(mb_deterministic_decision("don't"), 0);
    MARY_ASSERT_EQ(mb_deterministic_decision("yes, tighten it"), -1);
    MARY_ASSERT_EQ(mb_deterministic_decision(""), -1);
}

MARY_TEST(the_memory_plan_maps_the_gates_threads_onto_the_lanes_and_recall_gates_them) {
    ma_roster roster;
    ma_roster_maryos(&roster);
    ma_route *route = malloc(sizeof *route);
    ma_engine_inputs in = { .utterance = "how do I save in TextEdit", .classify_edit = true, .bare_decision = -2, .lead_application_id = "textedit", .roster = &roster, .now = 1000 };
    ma_engine_resolve(&in, route);
    mb_memory_plan plan;
    mb_memory_plan_for(route, NULL, "mary", &plan);
    MARY_ASSERT_EQ(plan.routing.lane_count, 1);           /* the routing habits; the skills themselves are the registry's */
    MARY_ASSERT_STR(plan.routing.lanes[0], "behavioral");
    MARY_ASSERT_EQ(plan.orchestration.lane_count, 1);
    MARY_ASSERT_STR(plan.orchestration.lanes[0], "behavioral");
    MARY_ASSERT_EQ(plan.context.lane_count, 1);           /* the ability thread: behavioral */
    MARY_ASSERT_STR(plan.context.lanes[0], "behavioral");
    MARY_ASSERT_STR(plan.lane_priority[0], "ability");
    MARY_ASSERT(plan.group_count >= 4);                   /* memory, files, style, the writing target's behaviour group */
    MARY_ASSERT_STR(plan.groups[0], "memory-mary");
    MARY_ASSERT_STR(plan.groups[2], "mary-style-mary");
    MARY_ASSERT(strncmp(plan.groups[3], "mary-ability-", 13) == 0);
    MARY_ASSERT(plan.entity_count >= 3);
    /* recall off for behavioral: the lane goes, the behaviour groups go */
    mb_recall recall = mb_recall_default();
    recall.behavioral = false;
    mb_memory_plan_for(route, &recall, "mary", &plan);
    MARY_ASSERT_EQ(plan.context.lane_count, 0);
    MARY_ASSERT_EQ(plan.routing.lane_count, 0);
    MARY_ASSERT_EQ(plan.group_count, 3);
    /* a chat: personal alone; a perceive turn adds behavioral */
    in.utterance = "how was your day";
    in.lead_application_id = NULL;
    ma_engine_resolve(&in, route);
    mb_memory_plan_for(route, NULL, "mary", &plan);
    MARY_ASSERT_EQ(plan.context.lane_count, 1);
    MARY_ASSERT_STR(plan.context.lanes[0], "personal");
    in.utterance = "what's on my screen";
    in.lead_application_id = "finder";
    ma_engine_resolve(&in, route);
    mb_memory_plan_for(route, NULL, "mary", &plan);
    MARY_ASSERT_EQ(plan.context.lane_count, 2);
    MARY_ASSERT_STR(plan.context.lanes[1], "behavioral");
    struct json_object *lanes = mb_purpose_json(&plan.context);
    MARY_ASSERT_STR(mc_json_compact(lanes, NULL), "[\"personal\",\"behavioral\"]");
    json_object_put(lanes);
    free(route);
}

int main(void) {
    struct json_object *msg = mc_json_parse(REGISTRY, strlen(REGISTRY));
    sk_registry_init(&registry);
    if (!msg || sk_registry_load(&registry, msg) != 0) { fprintf(stderr, "the fixture registry did not load\n"); return 1; }
    json_object_put(msg);
    MARY_RUN(the_index_embeds_every_skill_once_and_a_turn_finds_its_winner);
    MARY_RUN(the_shape_admits_no_arguments_one_string_or_one_spoken_enum);
    MARY_RUN(the_spoken_span_peels_the_preamble_the_app_and_the_trigger);
    MARY_RUN(the_span_drops_the_trailing_place_words_and_a_request_is_action_shaped_by_its_verb);
    MARY_RUN(a_zero_argument_verb_needs_a_whole_simple_sentence_and_bare_answers_are_exact);
    MARY_RUN(the_memory_plan_maps_the_gates_threads_onto_the_lanes_and_recall_gates_them);
    sk_registry_free(&registry);
    MARY_TEST_MAIN_END();
}
