/* The store's core over SQLite: deposits, groups, documents, removal, updates, stats. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/json.h"
#include "mary_test.h"
#include "thread/db.h"
#include "thread/store.h"

static void remove_tree(const char *path) {
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char child[512];
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            struct stat st;
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(child);
            else unlink(child);
        }
        closedir(dir);
    }
    rmdir(path);
}

static char dir[64];

static thread_store *fresh(void) {
    snprintf(dir, sizeof dir, "/tmp/thread-core-XXXXXX");
    MARY_ASSERT(mkdtemp(dir) != NULL);
    int error = 0;
    thread_store *s = thread_store_open(dir, &error);
    MARY_ASSERT(s != NULL);
    return s;
}

static void destroy(thread_store *s) {
    thread_store_close(s);
    remove_tree(dir);
}

static int put(thread_store *s, const char *owner, const char *group, const char *label, const char *id, const char *text, char *out, size_t *parts) {
    const char *texts[1] = { text };
    thread_deposit d = { .document_id = id, .group_id = group, .group_label = label, .texts = texts, .n_texts = text ? 1 : 0, .name = "a turn", .source = "threadctl" };
    return thread_store_deposit(s, owner, &d, out, parts);
}

MARY_TEST(a_turn_is_deposited_listed_read_back_and_kept_in_the_one_file) {
    thread_store *s = fresh();
    char id[THREAD_ID_MAX + 1];
    size_t parts = 0;
    MARY_ASSERT_EQ(put(s, "mary", "memory-mary", "Memory", "mary-turn-1", "What is the capital of France?", id, &parts), 0);
    MARY_ASSERT_STR(id, "mary-turn-1");
    MARY_ASSERT_EQ(parts, 1);
    MARY_ASSERT_EQ(put(s, "mary", "memory-mary", NULL, "mary-turn-2", "And of Portugal?", id, NULL), 0);
    struct json_object *lib = thread_store_library_json(s, "mary", 0, NULL, NULL, 0);
    struct json_object *groups = mc_json_array(lib, "groups");
    MARY_ASSERT_EQ(json_object_array_length(groups), 1);
    struct json_object *g = json_object_array_get_idx(groups, 0);
    MARY_ASSERT_STR(mc_json_string(g, "label"), "Memory");                 /* a later deposit without a label keeps it */
    MARY_ASSERT_STR(mc_json_string(g, "family"), "memory");
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(g, "documents")), 2);
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(mc_json_array(g, "documents"), 1), "family"), "memory");
    json_object_put(lib);
    const char *ids[] = { "mary-turn-2", "../etc/passwd", "no-such-turn", "mary-turn-1" };
    struct json_object *docs = thread_store_documents_json(s, "mary", ids, 4);
    struct json_object *arr = mc_json_array(docs, "documents");
    MARY_ASSERT_EQ(json_object_array_length(arr), 2);
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(arr, 0), "id"), "mary-turn-2");
    struct json_object *first = json_object_array_get_idx(arr, 1);
    MARY_ASSERT_STR(json_object_get_string(json_object_array_get_idx(mc_json_array(first, "texts"), 0)), "What is the capital of France?");
    MARY_ASSERT_STR(mc_json_string(first, "group_label"), "Memory");
    MARY_ASSERT_STR(mc_json_string(first, "enrich_state"), "pending");
    json_object_put(docs);
    /* one file, private */
    char path[128];
    struct stat st;
    snprintf(path, sizeof path, "%s/thread.db", dir);
    MARY_ASSERT_EQ(stat(path, &st), 0);
    MARY_ASSERT_EQ(st.st_mode & 0777, 0600);
    struct json_object *stats = thread_store_stats_json(s);
    int64_t n = 0;
    MARY_ASSERT(mc_json_int64(stats, "documents", &n) && n == 2);
    MARY_ASSERT(mc_json_int64(stats, "partitions", &n) && n == 2);
    MARY_ASSERT(mc_json_int64(stats, "jobs_pending", &n) && n == 2);
    MARY_ASSERT(mc_json_int64(stats, "entities", &n) && n >= 2);          /* keyword concepts until extraction */
    json_object_put(stats);
    struct json_object *schemas = thread_store_schemas_json(s);
    for (size_t i = 0; i < json_object_array_length(schemas); i++) {
        struct json_object *f = json_object_array_get_idx(schemas, i);
        if (strcmp(mc_json_string(f, "name"), "memory") == 0) {
            MARY_ASSERT(mc_json_int64(f, "count", &n) && n == 2);
            MARY_ASSERT(mc_json_int64(f, "last_written_ms", &n) && n > 0);
        }
    }
    json_object_put(schemas);
    destroy(s);
}

MARY_TEST(the_node_id_is_made_once_and_survives_reopening) {
    thread_store *s = fresh();
    char first[37];
    snprintf(first, sizeof first, "%s", thread_store_node_id(s));
    MARY_ASSERT_EQ(strlen(first), 36);
    MARY_ASSERT_EQ(first[14], '4');
    thread_store_close(s);
    int error = 0;
    s = thread_store_open(dir, &error);
    MARY_ASSERT_STR(thread_store_node_id(s), first);
    destroy(s);
}

MARY_TEST(one_owner_cannot_touch_anothers) {
    thread_store *s = fresh();
    MARY_ASSERT_EQ(put(s, "mary", "memory-mary", "Memory", "mary-turn-1", "private", NULL, NULL), 0);
    MARY_ASSERT_EQ(put(s, "guest", "memory-mary", NULL, "guest-turn-1", "hello", NULL, NULL), -EPERM);
    MARY_ASSERT_EQ(put(s, "guest", "guest-notes", NULL, "mary-turn-1", "overwrite", NULL, NULL), -EPERM);
    struct json_object *lib = thread_store_library_json(s, "guest", 0, NULL, NULL, 0);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(lib, "groups")), 0);
    json_object_put(lib);
    const char *ids[] = { "mary-turn-1" };
    struct json_object *docs = thread_store_documents_json(s, "guest", ids, 1);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(docs, "documents")), 0);
    json_object_put(docs);
    size_t removed = 9;
    MARY_ASSERT_EQ(thread_store_remove(s, "guest", ids, 1, "threadctl", &removed), 0);
    MARY_ASSERT_EQ(removed, 0);
    destroy(s);
}

MARY_TEST(ids_are_no_longer_file_names) {
    MARY_ASSERT(thread_id_valid("mary-turn-1757700000000-ab12"));
    MARY_ASSERT(thread_id_valid("a:b.c_d"));
    MARY_ASSERT(thread_id_valid("mary-routing-operate|media.play|1757700000"));
    MARY_ASSERT(thread_id_valid("\xC3\xA9tude"));
    MARY_ASSERT(!thread_id_valid(""));
    MARY_ASSERT(!thread_id_valid("a/b"));
    MARY_ASSERT(!thread_id_valid("with space"));
    MARY_ASSERT(!thread_id_valid("tab\there"));
    MARY_ASSERT(!thread_id_valid("\xC3"));
    thread_store *s = fresh();
    MARY_ASSERT_EQ(put(s, "mary", "../groups", NULL, "turn", "x", NULL, NULL), -EINVAL);
    MARY_ASSERT_EQ(put(s, "mary", "g", NULL, "../../escape", "x", NULL, NULL), -EINVAL);
    MARY_ASSERT_EQ(put(s, "mary", "g", NULL, "turn", NULL, NULL, NULL), -EINVAL);      /* nothing to store */
    destroy(s);
}

MARY_TEST(groups_page_by_id_and_a_document_moves_between_them) {
    thread_store *s = fresh();
    for (const char *g = "cab"; *g; g++) {
        char id[2] = { *g, 0 }, doc[8];
        snprintf(doc, sizeof doc, "doc-%c", *g);
        MARY_ASSERT_EQ(put(s, "mary", id, NULL, doc, "text", NULL, NULL), 0);
    }
    struct json_object *page = thread_store_library_json(s, "mary", 2, NULL, NULL, 0);
    struct json_object *groups = mc_json_array(page, "groups");
    MARY_ASSERT_EQ(json_object_array_length(groups), 2);
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(groups, 0), "id"), "a");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(groups, 1), "id"), "b");
    bool more = false;
    MARY_ASSERT(mc_json_bool(page, "has_more", &more) && more);
    json_object_put(page);
    page = thread_store_library_json(s, "mary", 2, "b", NULL, 0);
    groups = mc_json_array(page, "groups");
    MARY_ASSERT_EQ(json_object_array_length(groups), 1);
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(groups, 0), "id"), "c");
    MARY_ASSERT(mc_json_bool(page, "has_more", &more) && !more);
    json_object_put(page);
    const char *ids[] = { "doc-c" };
    page = thread_store_library_json(s, "mary", 0, NULL, ids, 1);
    groups = mc_json_array(page, "groups");
    MARY_ASSERT_EQ(json_object_array_length(groups), 1);
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(groups, 0), "id"), "c");
    json_object_put(page);
    /* a re-deposit into another group moves it and keeps created_at */
    MARY_ASSERT_EQ(put(s, "mary", "inbox", NULL, "note", "first", NULL, NULL), 0);
    int64_t created = thread_db_int(thread_store_db(s), "SELECT created_at FROM documents WHERE id = 'note'", NULL, NULL);
    MARY_ASSERT_EQ(put(s, "mary", "archive", NULL, "note", "second", NULL, NULL), 0);
    MARY_ASSERT_EQ(thread_db_int(thread_store_db(s), "SELECT created_at FROM documents WHERE id = 'note'", NULL, NULL), created);
    MARY_ASSERT_EQ(thread_db_int(thread_store_db(s), "SELECT revision FROM documents WHERE id = 'note'", NULL, NULL), 2);
    MARY_ASSERT_EQ(thread_db_int(thread_store_db(s), "SELECT COUNT(*) FROM documents WHERE group_id = 'inbox'", NULL, NULL), 0);
    MARY_ASSERT_EQ(thread_db_int(thread_store_db(s), "SELECT COUNT(*) FROM partitions WHERE document_id = 'note'", NULL, NULL), 1);
    const char *note[] = { "note" };
    struct json_object *docs = thread_store_documents_json(s, "mary", note, 1);
    MARY_ASSERT_STR(json_object_get_string(json_object_array_get_idx(mc_json_array(json_object_array_get_idx(mc_json_array(docs, "documents"), 0), "texts"), 0)), "second");
    json_object_put(docs);
    /* export pages by id and prefix */
    struct json_object *ex = thread_store_export_json(s, "mary", NULL, 0, "doc-", NULL, 2);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(ex, "documents")), 2);
    MARY_ASSERT(mc_json_bool(ex, "has_more", &more) && more);
    json_object_put(ex);
    ex = thread_store_export_json(s, "mary", NULL, 0, "doc-", "doc-b", 0);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(ex, "documents")), 1);
    json_object_put(ex);
    destroy(s);
}

MARY_TEST(a_chunked_text_a_computed_id_and_a_file_row) {
    thread_store *s = fresh();
    char *text = malloc(5000);
    text[0] = 0;
    for (int i = 0; i < 40; i++) strcat(text, "This sentence is exactly one hundred characters long when it is written out in full, honestly so. ");
    thread_deposit d = { .group_id = "files-mary", .group_label = "Files", .text = text, .chunk = true, .name = "essay.txt",
                         .file_path = "/home/mary/Documents/essay.txt", .file_kind = "document", .file_size = 4000, .file_mtime_ms = 1757700000000LL,
                         .file_hash = "abc", .file_text_indexed = true, .source = "indexd" };
    char id[THREAD_ID_MAX + 1];
    size_t parts = 0;
    MARY_ASSERT_EQ(thread_store_deposit(s, "mary", &d, id, &parts), 0);
    MARY_ASSERT(parts >= 3);
    MARY_ASSERT(strncmp(id, "file-", 5) == 0 && strlen(id) == 21);          /* a file's record is keyed by its path */
    /* the same file again is the same id; the family comes from the group */
    char again[THREAD_ID_MAX + 1];
    MARY_ASSERT_EQ(thread_store_deposit(s, "mary", &d, again, NULL), 0);
    MARY_ASSERT_STR(again, id);
    MARY_ASSERT_EQ(thread_db_int(thread_store_db(s), "SELECT COUNT(*) FROM documents", NULL, NULL), 1);
    char *family = thread_db_text(thread_store_db(s), "SELECT family FROM documents WHERE id = ?", id, NULL);
    MARY_ASSERT_STR(family, "file");
    free(family);
    MARY_ASSERT_EQ(thread_db_int(thread_store_db(s), "SELECT COUNT(*) FROM files WHERE path = '/home/mary/Documents/essay.txt'", NULL, NULL), 1);
    struct json_object *record = NULL;
    MARY_ASSERT_EQ(thread_store_file_record(s, "mary", "/home/mary/Documents/essay.txt", &record), 0);
    MARY_ASSERT(record != NULL);
    if (record) {
        MARY_ASSERT_EQ(json_object_array_length(mc_json_array(record, "partitions")), parts);
        MARY_ASSERT_STR(mc_json_string(mc_json_object(record, "file"), "kind"), "document");
        MARY_ASSERT(json_object_array_length(mc_json_array(record, "entities")) > 0);
        MARY_ASSERT(json_object_array_length(mc_json_array(record, "ledger")) >= 1);
        json_object_put(record);
    }
    MARY_ASSERT_EQ(thread_store_file_record(s, "mary", "/nowhere", &record), -ENOENT);
    /* metadata names the family and rides back as JSON */
    const char *meta = "{\"family\":\"routing\",\"intent\":\"operate\"}";
    const char *texts[] = { "play the song" };
    thread_deposit r = { .document_id = "mary-routing-operate|media.play|1", .group_id = "mary-routing-mary", .texts = texts, .n_texts = 1, .metadata = meta, .metadata_len = strlen(meta) };
    MARY_ASSERT_EQ(thread_store_deposit(s, "mary", &r, id, NULL), 0);
    const char *ids[] = { id };
    struct json_object *docs = thread_store_documents_json(s, "mary", ids, 1);
    struct json_object *doc = json_object_array_get_idx(mc_json_array(docs, "documents"), 0);
    MARY_ASSERT_STR(mc_json_string(doc, "family"), "routing");
    MARY_ASSERT_STR(mc_json_string(mc_json_object(doc, "metadata"), "intent"), "operate");
    json_object_put(docs);
    free(text);
    destroy(s);
}

MARY_TEST(the_json_deposit_removal_and_updates) {
    thread_store *s = fresh();
    const char *json = "{\"document_id\":\"mary-turn-9\",\"group\":\"memory-mary\",\"label\":\"Memory\",\"texts\":[\"hello\",\"hi\"],"
                       "\"entities\":[{\"name\":\"Mary\",\"kind\":\"person\"}],\"metadata\":{\"source\":\"typed\"},\"source\":\"maryd\",\"request_id\":\"mary-turn-9\"}";
    struct json_object *req = mc_json_parse(json, strlen(json)), *reply = NULL;
    MARY_ASSERT_EQ(thread_store_deposit_json(s, "mary", req, &reply), 0);
    MARY_ASSERT_STR(mc_json_string(reply, "document_id"), "mary-turn-9");
    MARY_ASSERT_STR(mc_json_string(reply, "family"), "memory");
    int64_t parts = 0;
    MARY_ASSERT(mc_json_int64(reply, "partitions", &parts) && parts == 2);
    json_object_put(reply);
    json_object_put(req);
    char mary_id[THREAD_GRAPH_ID_MAX];
    thread_entity_id("person", "Mary", mary_id);
    MARY_ASSERT_EQ(thread_db_int(thread_store_db(s), "SELECT mention_count FROM entities WHERE id = ?", mary_id, NULL), 1);
    /* updates */
    bool updated = false;
    const char *tags[] = { "chat" };
    MARY_ASSERT_EQ(thread_store_update_group(s, "mary", "memory-mary", "available", "Talks", "what was said", tags, 1, true, &updated), 0);
    MARY_ASSERT(updated);
    MARY_ASSERT_EQ(thread_store_update_group(s, "guest", "memory-mary", NULL, "Mine", NULL, NULL, 0, false, &updated), -EPERM);
    struct json_object *lib = thread_store_library_json(s, "mary", 0, NULL, NULL, 0);
    struct json_object *g = json_object_array_get_idx(mc_json_array(lib, "groups"), 0);
    MARY_ASSERT_STR(mc_json_string(g, "label"), "Talks");
    MARY_ASSERT_STR(mc_json_string(g, "access"), "available");
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(g, "tags")), 1);
    json_object_put(lib);
    thread_deposit other = { .document_id = "x", .group_id = "other", .texts = tags, .n_texts = 1 };
    MARY_ASSERT_EQ(thread_store_deposit(s, "mary", &other, NULL, NULL), 0);
    MARY_ASSERT_EQ(thread_store_update_document(s, "mary", "mary-turn-9", "restricted", "other", &updated), 0);
    MARY_ASSERT(updated);
    char *group = thread_db_text(thread_store_db(s), "SELECT group_id FROM documents WHERE id = 'mary-turn-9'", NULL, NULL);
    MARY_ASSERT_STR(group, "other");
    free(group);
    /* removal takes the graph with it */
    const char *ids[] = { "mary-turn-9" };
    size_t removed = 0;
    MARY_ASSERT_EQ(thread_store_remove(s, "mary", ids, 1, "threadctl", &removed), 0);
    MARY_ASSERT_EQ(removed, 1);
    MARY_ASSERT_EQ(thread_db_int(thread_store_db(s), "SELECT COUNT(*) FROM entities WHERE id = ?", mary_id, NULL), 0);
    MARY_ASSERT_EQ(thread_db_int(thread_store_db(s), "SELECT COUNT(*) FROM partitions", NULL, NULL), 1);
    MARY_ASSERT_EQ(thread_store_remove(s, "mary", NULL, 0, "threadctl", &removed), 0);
    MARY_ASSERT_EQ(removed, 1);
    MARY_ASSERT_EQ(thread_db_int(thread_store_db(s), "SELECT COUNT(*) FROM documents", NULL, NULL), 0);
    destroy(s);
}

int main(void) {
    MARY_RUN(a_turn_is_deposited_listed_read_back_and_kept_in_the_one_file);
    MARY_RUN(the_node_id_is_made_once_and_survives_reopening);
    MARY_RUN(one_owner_cannot_touch_anothers);
    MARY_RUN(ids_are_no_longer_file_names);
    MARY_RUN(groups_page_by_id_and_a_document_moves_between_them);
    MARY_RUN(a_chunked_text_a_computed_id_and_a_file_row);
    MARY_RUN(the_json_deposit_removal_and_updates);
    MARY_TEST_MAIN_END();
}
