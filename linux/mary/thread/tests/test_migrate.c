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
    /* a note of the first release's memory group comes across; the conversations stay behind, retired */
    snprintf(path, sizeof path, "%s/groups/memory-mary.json", test_dir);
    const char *mg = "{\"id\":\"memory-mary\",\"label\":\"Memory\",\"owner_id\":\"mary\",\"created_at\":1757700000,\"documents\":[\"note-1\"]}";
    mc_write_file_atomic(path, mg, strlen(mg), 0600);
    snprintf(path, sizeof path, "%s/documents/note-1.json", test_dir);
    const char *md = "{\"id\":\"note-1\",\"owner_id\":\"mary\",\"group_id\":\"memory-mary\",\"name\":\"Paris in spring\",\"media_type\":\"text/plain\","
                     "\"created_at\":1757700002,\"metadata\":\"eyJzb3VyY2UiOiJ2b2ljZSJ9\",\"texts\":[\"You planned Paris.\",\"In spring.\"]}";
    mc_write_file_atomic(path, md, strlen(md), 0600);
    thread_store_options o = { .dir = test_dir, .embedder = &FAKE, .dim = TEST_DIM };
    int error = 0;
    thread_store *s = thread_store_open_with(&o, &error);
    MARY_ASSERT(s != NULL);
    MARY_ASSERT_STR(thread_store_node_id(s), "12345678-1234-4234-8234-123456789abc");
    sqlite3 *db = thread_store_db(s);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM documents WHERE id = 'mary-turn-1'", NULL, NULL), 0);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM groups WHERE id = 'mary-conversations' OR id = 'conversation-mary'", NULL, NULL), 0);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM documents WHERE id = 'note-1' AND family = 'memory' AND group_id = 'memory-mary'", NULL, NULL), 1);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM partitions WHERE document_id = 'note-1'", NULL, NULL), 2);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM jobs", NULL, NULL), 1);
    const char *ids[] = { "note-1" };
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

MARY_TEST(a_file_from_before_the_retirement_is_cleaned_at_open) {
    snprintf(test_dir, sizeof test_dir, "/tmp/thread-test-XXXXXX");
    MARY_ASSERT(mkdtemp(test_dir) != NULL);
    thread_store_options o = { .dir = test_dir, .embedder = &FAKE, .dim = TEST_DIM };
    int error = 0;
    thread_store *s = thread_store_open_with(&o, &error);
    MARY_ASSERT(s != NULL);
    /* what a version-1 file held: an ability record, a turn, an interaction stub, beside a memory that stays */
    sqlite3 *db = thread_store_db(s);
    const char *seed =
        "INSERT INTO groups(id, owner, label, created_at) VALUES('mary-ability-abc', 'mary', 'Ability', 1), ('conversation-mary', 'mary', 'Conversation', 1),"
        " ('mary-behavior-interaction-mary', 'mary', 'Interactions', 1), ('memory-mary', 'mary', 'Memory', 1);"
        "INSERT INTO documents(id, owner, group_id, family, name, media_type, metadata, content_hash, created_at, updated_ms) VALUES"
        " ('mary-ability-schema-1', 'mary', 'mary-ability-abc', 'ability', 'Open', 'text/plain', '{}', 'h1', 1, 1),"
        " ('mary-turn-1', 'mary', 'conversation-mary', 'conversation', 'What time is it?', 'text/plain', '{}', 'h2', 1, 1),"
        " ('mary-behavior-interaction-1', 'mary', 'mary-behavior-interaction-mary', 'interaction', 'Interaction', 'text/plain', '{}', 'h3', 1, 1),"
        " ('note-1', 'mary', 'memory-mary', 'memory', 'Paris', 'text/plain', '{}', 'h4', 1, 1);"
        "PRAGMA user_version = 1;";
    MARY_ASSERT_EQ(thread_db_exec(db, seed), 0);
    thread_store_close(s);
    s = thread_store_open_with(&o, &error);
    MARY_ASSERT(s != NULL);
    db = thread_store_db(s);
    MARY_ASSERT_EQ(thread_db_int(db, "PRAGMA user_version", NULL, NULL), 2);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM documents WHERE family IN ('ability', 'conversation', 'interaction')", NULL, NULL), 0);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM documents WHERE id = 'note-1'", NULL, NULL), 1);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM groups WHERE id IN ('mary-ability-abc', 'conversation-mary', 'mary-behavior-interaction-mary')", NULL, NULL), 0);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM groups WHERE id = 'memory-mary'", NULL, NULL), 1);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT count FROM ledger WHERE kind = 'mutation' ORDER BY id DESC LIMIT 1", NULL, NULL), 3);
    thread_store_close(s);
    /* a second open finds nothing to do */
    s = thread_store_open_with(&o, &error);
    MARY_ASSERT(s != NULL);
    MARY_ASSERT_EQ(thread_db_int(thread_store_db(s), "SELECT COUNT(*) FROM ledger WHERE kind = 'mutation'", NULL, NULL), 1);
    close_store(s);
}

int main(void) {
    MARY_RUN(the_legacy_json_files_are_imported_once_and_moved_aside);
    MARY_RUN(a_file_from_before_the_retirement_is_cleaned_at_open);
    MARY_TEST_MAIN_END();
}
