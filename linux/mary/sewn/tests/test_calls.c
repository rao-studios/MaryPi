#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/json.h"
#include "mary_test.h"
#include "sewn/calls.h"

static char dir[64];

static void one(sewn_calls *c, const char *purpose, long status) {
    sewn_call_row row = { 0 };
    snprintf(row.provider, sizeof row.provider, "mistral");
    snprintf(row.host, sizeof row.host, "api.mistral.ai");
    snprintf(row.path, sizeof row.path, "/v1/chat/completions");
    snprintf(row.purpose, sizeof row.purpose, "%s", purpose);
    row.status = status;
    row.ms = 12;
    row.bytes_out = 100;
    row.bytes_in = 50;
    sewn_calls_record(c, &row);
}

MARY_TEST(the_ring_keeps_the_newest_and_the_file_rotates) {
    snprintf(dir, sizeof dir, "/tmp/sewn-calls-XXXXXX");
    MARY_ASSERT(mkdtemp(dir) != NULL);
    sewn_calls c;
    sewn_calls_init(&c, dir);
    for (int i = 0; i < SEWN_CALLS_RING + 10; i++) one(&c, i % 3 ? "chat" : "embed", i % 7 ? 200 : 429);
    struct json_object *list = sewn_calls_list(&c, 0);
    MARY_ASSERT_EQ(json_object_array_length(list), SEWN_CALLS_RING);
    int64_t first = 0, second = 0;
    mc_json_int64(json_object_array_get_idx(list, 0), "id", &first);
    mc_json_int64(json_object_array_get_idx(list, 1), "id", &second);
    MARY_ASSERT_EQ(first, SEWN_CALLS_RING + 10);            /* newest first */
    MARY_ASSERT_EQ(second, SEWN_CALLS_RING + 9);
    json_object_put(list);
    list = sewn_calls_list(&c, 3);
    MARY_ASSERT_EQ(json_object_array_length(list), 3);
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(list, 0), "outcome"), "ok");
    json_object_put(list);

    struct json_object *stats = sewn_calls_stats(&c);
    int64_t count = 0, failed = 0;
    mc_json_int64(stats, "count", &count);
    mc_json_int64(stats, "failed", &failed);
    MARY_ASSERT_EQ(count, SEWN_CALLS_RING);
    MARY_ASSERT(failed > 0);
    MARY_ASSERT(mc_json_object(stats, "by_purpose") != NULL);
    json_object_put(stats);

    char path[128];
    snprintf(path, sizeof path, "%s/calls.jsonl", dir);
    struct stat st;
    MARY_ASSERT_EQ(stat(path, &st), 0);
    MARY_ASSERT_EQ(st.st_mode & 0777, 0600);
    /* rotation: grow the file past the limit */
    for (int i = 0; i < 12000; i++) one(&c, "speech", 200);
    char rotated[128];
    snprintf(rotated, sizeof rotated, "%s/calls.jsonl.1", dir);
    MARY_ASSERT_EQ(stat(rotated, &st), 0);
    MARY_ASSERT(st.st_size >= (off_t)SEWN_CALLS_FILE_MAX);
    MARY_ASSERT_EQ(stat(path, &st), 0);
    MARY_ASSERT(st.st_size < (off_t)SEWN_CALLS_FILE_MAX);
    /* never a key or a body: the row has no field for one */
    FILE *f = fopen(path, "r");
    char line[1024];
    MARY_ASSERT(fgets(line, sizeof line, f) != NULL);
    fclose(f);
    MARY_ASSERT(strstr(line, "\"purpose\"") != NULL);
    MARY_ASSERT(strstr(line, "body") == NULL && strstr(line, "key") == NULL);
    sewn_calls_free(&c);
    unlink(path);
    unlink(rotated);
    rmdir(dir);
}

int main(void) {
    MARY_RUN(the_ring_keeps_the_newest_and_the_file_rotates);
    MARY_TEST_MAIN_END();
}
