#include <stdlib.h>
#include <string.h>

#include "common/json.h"
#include "mary_test.h"
#include "sewn/compact.h"

static sewn_partition part(const char *doc, const char *name, const char *text, const char *owner, const char *family, const char *lane) {
    sewn_partition p = { 0 };
    p.partition_id = (char *)doc;
    p.document_id = (char *)doc;
    p.name = (char *)name;
    p.text = (char *)text;
    p.owner_id = (char *)owner;
    p.family = (char *)family;
    p.lane = (char *)lane;
    p.group_id = "g";
    return p;
}

MARY_TEST(the_verbatim_block_tiers_by_family_and_tags_in_order) {
    sewn_partition ps[5] = {
        part("m1", "Paris trip", "You planned Paris.", "rao", "memory", "personal"),
        part("f1", "notes.txt", "Buy bread.", "rao", "file", "personal"),
        part("y1", "Writing", "Discipline: Writing. Realized by TextEdit.", "rao", "style", "personal"),
        part("b1", "Behaviour: play the music", "{\"schema\":\"mary.behavior\"}", "rao", "behavior", "behavioral"),
        part("s1", "someone's note", "Bread is good.", "guest", "file", "personal"),
    };
    struct json_object *index = NULL;
    char *text = sewn_compact_verbatim(ps, 5, "rao", &index);
    MARY_ASSERT_STR(text,
        "**Memory:**\n[1] \"Paris trip\"\nYou planned Paris.\n\n"
        "**Documents:**\n[2] \"notes.txt\"\nBuy bread.\n\n[3] \"Writing\"\nDiscipline: Writing. Realized by TextEdit.\n\n"
        "**Mary's Past Actions (her own records of what the applications can do and what she did \xE2\x80\x94 background, not user prose):**\n[4] \"Behaviour: play the music\"\n{\"schema\":\"mary.behavior\"}\n\n"
        "**Perspectives From Others:**\n<external>\n[5] \"someone's note\"\nBread is good.\n</external>");
    MARY_ASSERT_EQ(json_object_array_length(index), 5);
    MARY_ASSERT_STR(json_object_get_string(json_object_array_get_idx(index, 2)), "y1");
    free(text);
    json_object_put(index);
    text = sewn_compact_verbatim(ps, 0, "rao", NULL);
    MARY_ASSERT_STR(text, "**Memory:**\nNone.\n\n**Documents:**\nNone.\n\n**Perspectives From Others:**\n<external>\nNone.\n</external>");
    free(text);
}

MARY_TEST(small_retrievals_are_verbatim_and_large_ones_need_the_briefing) {
    char big[7000];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    sewn_partition small = part("d", "n", "short", "rao", "file", "personal"), large = part("d", "n", big, "rao", "file", "personal");
    MARY_ASSERT_EQ(sewn_partitions_chars(&small, 1), 5);
    MARY_ASSERT(sewn_partitions_chars(&large, 1) > SEWN_VERBATIM_CONTEXT_THRESHOLD);
    sewn_service svc;
    sewn_service_init(&svc, "/tmp");
    svc.post_stream = NULL;                          /* no transport: the briefing cannot run */
    sewn_compact_result r;
    char message[128] = "";
    MARY_ASSERT_EQ(sewn_compact(&svc, "k", SEWN_PROVIDER_MISTRAL, NULL, &small, 1, "rao", NULL, &r, message, sizeof message), 0);
    MARY_ASSERT(r.used_verbatim);
    MARY_ASSERT(strstr(r.text, "[1] \"n\"") != NULL);
    sewn_compact_result_free(&r);
    MARY_ASSERT(sewn_compact(&svc, "k", SEWN_PROVIDER_MISTRAL, NULL, &large, 1, "rao", NULL, &r, message, sizeof message) < 0);
    sewn_service_free(&svc);
    const char *msgs = "[{\"role\":\"user\",\"content\":\"hi\"},{\"role\":\"assistant\",\"content\":\"hello\"}]";
    struct json_object *messages = mc_json_parse(msgs, strlen(msgs));
    char *input = sewn_compact_briefing_input(messages, &small, 1, "rao");
    MARY_ASSERT(strncmp(input, "**Message History:**\nUser: hi\nAssistant: hello\n\n**THE USER'S MEMORY", 60) == 0);
    MARY_ASSERT(strstr(input, "**THE USER'S DOCUMENTS (personal notes and files):**\n[1] \"n\"\nshort") != NULL);
    free(input);
    json_object_put(messages);
    MARY_ASSERT(strstr(sewn_compact_briefing_prompt(), "### Conversation History") != NULL);
    MARY_ASSERT(strstr(sewn_memory_instruction(true), "no retrieved memories") != NULL);
    MARY_ASSERT(strstr(sewn_memory_instruction(false), "draw on them specifically") != NULL);
    char *guide = sewn_context_usage_guide("- protocol");
    MARY_ASSERT(strstr(guide, "**How to use this:**") == guide);
    MARY_ASSERT(strstr(guide, "- protocol") != NULL);
    free(guide);
}

int main(void) {
    MARY_RUN(the_verbatim_block_tiers_by_family_and_tags_in_order);
    MARY_RUN(small_retrievals_are_verbatim_and_large_ones_need_the_briefing);
    MARY_TEST_MAIN_END();
}
