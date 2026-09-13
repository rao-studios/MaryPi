#include "test_support.h"
#include "common/io.h"

MARY_TEST(the_legacy_json_files_are_imported_once_and_moved_aside) {
    snprintf(test_dir, sizeof test_dir, "/tmp/thread-test-XXXXXX");
    MARY_ASSERT(mkdtemp(test_dir) != NULL);
    char path[256];
    snprintf(path, sizeof path, "%s/groups", test_dir);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/documents", test_dir);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/node-id", test_dir);
    mc_write_file_atomic(path, "12345678-1234-4234-8234-123456789abc\n", 37, 0600);
    snprintf(path, sizeof path, "%s/groups/mary-conversations.json", test_dir);
    const char *g = "{\"id\":\"mary-conversations\",\"label\":\"Conversations with Mary\",\"owner_id\":\"mary\",\"created_at\":1757700000,\"documents\":[\"mary-turn-1\"]}";
    mc_write_file_atomic(path, g, strlen(g), 0600);
    snprintf(path, sizeof path, "%s/documents/mary-turn-1.json", test_dir);
    const char *d = "{\"id\":\"mary-turn-1\",\"owner_id\":\"mary\",\"group_id\":\"mary-conversations\",\"name\":\"What time is it?\",\"media_type\":\"text/plain\","
                    "\"created_at\":1757700001,\"metadata\":\"eyJzb3VyY2UiOiJ2b2ljZSJ9\",\"texts\":[\"What time is it?\",\"Half past four.\"]}";
    mc_write_file_atomic(path, d, strlen(d), 0600);
    thread_store_options o = { .dir = test_dir, .embedder = &FAKE, .dim = TEST_DIM };
    int error = 0;
    thread_store *s = thread_store_open_with(&o, &error);
    MARY_ASSERT(s != NULL);
    MARY_ASSERT_STR(thread_store_node_id(s), "12345678-1234-4234-8234-123456789abc");
    sqlite3 *db = thread_store_db(s);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM documents WHERE id = 'mary-turn-1' AND family = 'conversation' AND group_id = 'conversation-mary'", NULL, NULL), 1);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM groups WHERE id = 'conversation-mary' AND label = 'Conversations with Mary'", NULL, NULL), 1);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM partitions WHERE document_id = 'mary-turn-1'", NULL, NULL), 2);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM jobs", NULL, NULL), 1);
    const char *ids[] = { "mary-turn-1" };
    struct json_object *docs = thread_store_documents_json(s, "mary", ids, 1);
    struct json_object *doc = json_object_array_get_idx(mc_json_array(docs, "documents"), 0);
    MARY_ASSERT_STR(mc_json_string(mc_json_object(doc, "metadata"), "source"), "voice");
    MARY_ASSERT_STR(mc_json_string(doc, "media_type"), "text/plain");
    json_object_put(docs);
    struct stat st;
    snprintf(path, sizeof path, "%s/legacy/documents/mary-turn-1.json", test_dir);
    MARY_ASSERT_EQ(stat(path, &st), 0);
    snprintf(path, sizeof path, "%s/documents", test_dir);
    MARY_ASSERT(stat(path, &st) != 0);
    thread_store_close(s);
    /* a second open imports nothing more */
    s = thread_store_open_with(&o, &error);
    MARY_ASSERT(s != NULL);
    MARY_ASSERT_EQ(thread_db_int(thread_store_db(s), "SELECT COUNT(*) FROM documents", NULL, NULL), 1);
    MARY_ASSERT_STR(thread_store_node_id(s), "12345678-1234-4234-8234-123456789abc");
    close_store(s);
}

int main(void) {
    MARY_RUN(the_legacy_json_files_are_imported_once_and_moved_aside);
    MARY_TEST_MAIN_END();
}
