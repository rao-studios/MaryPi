#include "common/json.h"
#include "mary_test.h"
#include "thread/families.h"

MARY_TEST(every_family_has_a_lane_a_writer_and_fields) {
    for (size_t i = 0; i < thread_family_count; i++) {
        const thread_family *f = &thread_families[i];
        MARY_ASSERT(f->name && *f->name && f->label && *f->label && f->description && *f->description);
        MARY_ASSERT(f->writer && *f->writer);
        if (strcmp(f->name, "unknown") != 0) MARY_ASSERT(thread_lane_valid(f->lane));
        struct json_object *fields = mc_json_parse(f->fields, strlen(f->fields));
        MARY_ASSERT(fields && json_object_is_type(fields, json_type_array));
        json_object_put(fields);
    }
    MARY_ASSERT(thread_family_named("conversation") != NULL);
    MARY_ASSERT(thread_family_named("nope") == NULL);
    MARY_ASSERT_STR(thread_family_lane("file"), "personal");
    MARY_ASSERT_STR(thread_family_lane("behavior"), "behavioral");
    MARY_ASSERT_STR(thread_family_lane("ability"), "application");
    MARY_ASSERT_STR(thread_family_lane("nope"), "");
    MARY_ASSERT(thread_lane_valid("conversation") && !thread_lane_valid("everything"));
}

MARY_TEST(classification_prefers_metadata_then_the_longest_prefix) {
    MARY_ASSERT_STR(thread_family_of("mary-behavior-interaction-abc", "mary-behavior-interaction-rao", NULL, 0), "interaction");
    MARY_ASSERT_STR(thread_family_of("mary-behavior-abc", "mary-ability-1234", NULL, 0), "behavior");
    MARY_ASSERT_STR(thread_family_of("mary-ability-schema-manifest-1", "mary-ability-1", NULL, 0), "ability-schema");
    MARY_ASSERT_STR(thread_family_of("mary-ability-schema-1", "mary-ability-1", NULL, 0), "ability");
    MARY_ASSERT_STR(thread_family_of("mary-turn-1757700000000-ab12", "conversation-rao", NULL, 0), "conversation");
    MARY_ASSERT_STR(thread_family_of("mary-turn-1", "mary-conversations", NULL, 0), "conversation");
    MARY_ASSERT_STR(thread_family_of("227176196", "memory-rao", NULL, 0), "memory");
    MARY_ASSERT_STR(thread_family_of("file-af63dc4c8601ec8c", "files-rao", NULL, 0), "file");
    MARY_ASSERT_STR(thread_family_of("something", "somewhere", NULL, 0), "unknown");
    const char *meta = "{\"family\":\"routing\"}";
    MARY_ASSERT_STR(thread_family_of("something", "somewhere", meta, strlen(meta)), "routing");
    const char *bad = "{\"family\":\"nope\"}";
    MARY_ASSERT_STR(thread_family_of("file-x", "", bad, strlen(bad)), "file");
}

MARY_TEST(the_schemas_render_as_json) {
    struct json_object *arr = thread_families_json();
    MARY_ASSERT_EQ(json_object_array_length(arr), thread_family_count);
    struct json_object *first = json_object_array_get_idx(arr, 0);
    MARY_ASSERT_STR(mc_json_string(first, "name"), "file");
    MARY_ASSERT(mc_json_array(first, "fields") && json_object_array_length(mc_json_array(first, "fields")) >= 5);
    json_object_put(arr);
}

int main(void) {
    MARY_RUN(every_family_has_a_lane_a_writer_and_fields);
    MARY_RUN(classification_prefers_metadata_then_the_longest_prefix);
    MARY_RUN(the_schemas_render_as_json);
    MARY_TEST_MAIN_END();
}
