#include "test_support.h"
#include "foundation/hash.h"

static int file_deposit(thread_store *s, const char *path, const char *text, const char *hash) {
    char key[512], hex[17], id[64];
    snprintf(key, sizeof key, "mary|%s", path);
    mf_fnv1a64_hex(key, hex);
    snprintf(id, sizeof id, "file-%s", hex);
    thread_deposit d = { .document_id = id, .group_id = "files-mary", .text = text, .chunk = true, .name = path,
                         .file_path = path, .file_kind = "document", .file_size = (int64_t)strlen(text), .file_mtime_ms = 1000, .file_hash = hash,
                         .file_text_indexed = true, .source = "indexd" };
    return thread_store_deposit(s, "mary", &d, NULL, NULL);
}

static struct json_object *entry(const char *path, int64_t size, const char *hash) {
    struct json_object *e = json_object_new_object();
    json_object_object_add(e, "path", json_object_new_string(path));
    json_object_object_add(e, "size", json_object_new_int64(size));
    json_object_object_add(e, "mtime_ms", json_object_new_int64(1000));
    json_object_object_add(e, "hash", json_object_new_string(hash));
    return e;
}

MARY_TEST(a_two_page_report_classifies_and_removes_the_orphan_with_its_entity) {
    thread_store *s = open_store(true);
    MARY_ASSERT_EQ(file_deposit(s, "/home/mary/a.txt", "alpha notes", "ha"), 0);
    MARY_ASSERT_EQ(file_deposit(s, "/home/mary/b.txt", "beta notes", "hb"), 0);
    MARY_ASSERT_EQ(file_deposit(s, "/home/mary/gone.txt", "an orphan with a unique zebra", "hz"), 0);
    thread_store_enrich_drain(s);
    sqlite3 *db = thread_store_db(s);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM entities WHERE name = 'zebra'", NULL, NULL), 1);
    int64_t run = 0;
    struct json_object *page = json_object_new_array(), *report = NULL;
    json_object_array_add(page, entry("/home/mary/a.txt", 11, "ha"));           /* recorded */
    json_object_array_add(page, entry("/home/mary/b.txt", 99, "hb"));           /* stale: the size moved */
    MARY_ASSERT_EQ(thread_store_parity_report(s, "mary", &run, page, false, &report), 0);
    MARY_ASSERT(run > 0);
    int64_t n = 0;
    MARY_ASSERT(mc_json_int64(report, "recorded", &n) && n == 1);
    MARY_ASSERT(mc_json_int64(report, "stale", &n) && n == 1);
    json_object_put(report);
    json_object_put(page);
    page = json_object_new_array();
    json_object_array_add(page, entry("/home/mary/new.txt", 5, "hn"));          /* missing */
    MARY_ASSERT_EQ(thread_store_parity_report(s, "mary", &run, page, true, &report), 0);
    MARY_ASSERT(mc_json_int64(report, "seen", &n) && n == 3);
    MARY_ASSERT(mc_json_int64(report, "missing", &n) && n == 1);
    MARY_ASSERT(mc_json_int64(report, "orphaned", &n) && n == 1);
    struct json_object *entries = mc_json_array(report, "entries");
    MARY_ASSERT_EQ(json_object_array_length(entries), 2);                        /* this page's missing, plus the orphan */
    json_object_put(report);
    json_object_put(page);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM files", NULL, NULL), 2);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM entities WHERE name = 'zebra'", NULL, NULL), 0);   /* detached with the orphan */
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM ledger WHERE kind = 'reconcile'", NULL, NULL), 1);
    struct json_object *last = thread_store_parity_last(s, "mary");
    MARY_ASSERT(mc_json_int64(last, "orphaned", &n) && n == 1);
    MARY_ASSERT(mc_json_int64(last, "files", &n) && n == 2);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(last, "entries")), 3);   /* both pages' non-recorded rows */
    json_object_put(last);
    close_store(s);
}

MARY_TEST(a_move_re_keys_the_record_and_a_removal_detaches_it) {
    thread_store *s = open_store(true);
    MARY_ASSERT_EQ(file_deposit(s, "/home/mary/draft.txt", "a draft mentioning quokkas", "hd"), 0);
    thread_store_enrich_drain(s);
    sqlite3 *db = thread_store_db(s);
    char *old_pid = thread_db_text(db, "SELECT id FROM partitions LIMIT 1", NULL, NULL);
    MARY_ASSERT_EQ(thread_store_file_move(s, "mary", "/home/mary/draft.txt", "/home/mary/final.txt", "indexd"), 0);
    MARY_ASSERT_EQ(thread_store_file_move(s, "mary", "/home/mary/draft.txt", "/home/mary/x.txt", "indexd"), -ENOENT);
    char key[256], hex[17], id[64];
    snprintf(key, sizeof key, "mary|/home/mary/final.txt");
    mf_fnv1a64_hex(key, hex);
    snprintf(id, sizeof id, "file-%s", hex);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM documents WHERE id = ?", id, NULL), 1);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM files WHERE path = '/home/mary/final.txt' AND id = ?", id, NULL), 1);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM partitions WHERE document_id = ?", id, NULL), 1);
    char *new_pid = thread_db_text(db, "SELECT id FROM partitions LIMIT 1", NULL, NULL);
    MARY_ASSERT(old_pid && new_pid && strcmp(old_pid, new_pid) != 0);       /* the partition id hashes the document id */
    free(old_pid);
    free(new_pid);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM entity_documents WHERE document_id = ?", id, NULL) > 0, 1);
    struct json_object *record = NULL;
    MARY_ASSERT_EQ(thread_store_file_record(s, "mary", "/home/mary/final.txt", &record), 0);
    MARY_ASSERT_STR(mc_json_string(mc_json_object(record, "document"), "id"), id);
    json_object_put(record);
    /* the vectors followed: a search still finds it */
    struct json_object *r = NULL;
    thread_search_request q = { .query_text = "quokkas", .top_k = 1 };
    MARY_ASSERT_EQ(thread_store_search(s, "mary", &q, &r), 0);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(r, "results")), 1);
    json_object_put(r);
    bool removed = false;
    MARY_ASSERT_EQ(thread_store_file_remove(s, "mary", "/home/mary/final.txt", "indexd", &removed), 0);
    MARY_ASSERT(removed);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM documents", NULL, NULL), 0);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM entities", NULL, NULL), 0);
    MARY_ASSERT_EQ(thread_store_file_remove(s, "mary", "/home/mary/final.txt", "indexd", &removed), 0);
    MARY_ASSERT(!removed);
    close_store(s);
}

MARY_TEST(a_deposit_with_a_path_is_keyed_by_the_file_and_replaces_itself) {
    thread_store *s = open_store(false);
    const char *one = "{\"group\":\"files-mary\",\"text\":\"one\",\"file\":{\"path\":\"/home/mary/a.txt\",\"kind\":\"text\",\"size\":3}}";
    struct json_object *first = mc_json_parse(one, strlen(one));
    struct json_object *reply = NULL;
    MARY_ASSERT_EQ(thread_store_deposit_json(s, "mary", first, &reply), 0);
    char id[THREAD_ID_MAX + 1];
    snprintf(id, sizeof id, "%s", mc_json_string(reply, "document_id"));
    MARY_ASSERT(strncmp(id, "file-", 5) == 0);
    MARY_ASSERT_STR(mc_json_string(reply, "family"), "file");
    json_object_put(reply);
    const char *two = "{\"group\":\"files-mary\",\"text\":\"two words\",\"file\":{\"path\":\"/home/mary/a.txt\",\"kind\":\"text\",\"size\":9}}";
    struct json_object *second = mc_json_parse(two, strlen(two));
    MARY_ASSERT_EQ(thread_store_deposit_json(s, "mary", second, &reply), 0);
    MARY_ASSERT_STR(mc_json_string(reply, "document_id"), id);           /* the same file, the same record */
    json_object_put(reply);
    struct json_object *stats = thread_store_stats_json(s);
    int64_t docs = 0;
    mc_json_int64(stats, "documents", &docs);
    MARY_ASSERT_EQ(docs, 1);
    json_object_put(stats);
    json_object_put(first);
    json_object_put(second);
    close_store(s);
}

int main(void) {
    MARY_RUN(a_deposit_with_a_path_is_keyed_by_the_file_and_replaces_itself);
    MARY_RUN(a_two_page_report_classifies_and_removes_the_orphan_with_its_entity);
    MARY_RUN(a_move_re_keys_the_record_and_a_removal_detaches_it);
    MARY_TEST_MAIN_END();
}
