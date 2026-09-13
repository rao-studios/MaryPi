#include <stdlib.h>

#include "common/json.h"
#include "mary_test.h"
#include "thread/db.h"
#include "thread/ledger.h"

MARY_TEST(rows_page_newest_first_with_their_documents_and_filters) {
    sqlite3 *db = NULL;
    MARY_ASSERT_EQ(thread_db_open(":memory:", &db), 0);
    thread_ledger_row search = { .kind = "search", .source = "maryd", .request_id = "req-1", .count = 2, .ms = 12 };
    int64_t id = thread_ledger_record(db, &search);
    MARY_ASSERT(id > 0);
    MARY_ASSERT_EQ(thread_ledger_record_hit(db, id, 0, "doc-a", "p1", 1.5), 0);
    MARY_ASSERT_EQ(thread_ledger_record_hit(db, id, 1, "doc-b", "p2", 2.5), 0);
    struct json_object *detail = json_object_new_object();
    json_object_object_add(detail, "chunks", json_object_new_int(3));
    thread_ledger_row index = { .kind = "index", .source = "indexd", .document_id = "doc-a", .group_id = "files-mary", .count = 1, .detail = detail };
    MARY_ASSERT(thread_ledger_record(db, &index) > id);
    json_object_put(detail);

    struct json_object *rows = NULL;
    int more = 1;
    thread_ledger_filter all = { 0 };
    MARY_ASSERT_EQ(thread_ledger_page(db, &all, &rows, &more), 0);
    MARY_ASSERT_EQ(json_object_array_length(rows), 2);
    MARY_ASSERT(!more);
    struct json_object *first = json_object_array_get_idx(rows, 0);
    MARY_ASSERT_STR(mc_json_string(first, "kind"), "index");
    int64_t chunks = 0;
    MARY_ASSERT(mc_json_int64(mc_json_object(first, "detail"), "chunks", &chunks) && chunks == 3);
    struct json_object *second = json_object_array_get_idx(rows, 1);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(second, "documents")), 2);
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(mc_json_array(second, "documents"), 1), "document_id"), "doc-b");
    json_object_put(rows);

    thread_ledger_filter by_request = { .request_id = "req-1" };
    MARY_ASSERT_EQ(thread_ledger_page(db, &by_request, &rows, &more), 0);
    MARY_ASSERT_EQ(json_object_array_length(rows), 1);
    json_object_put(rows);
    thread_ledger_filter by_kind = { .kind = "index", .limit = 1 };
    MARY_ASSERT_EQ(thread_ledger_page(db, &by_kind, &rows, &more), 0);
    MARY_ASSERT_EQ(json_object_array_length(rows), 1);
    MARY_ASSERT(!more);
    json_object_put(rows);
    thread_ledger_filter paged = { .limit = 1 };
    MARY_ASSERT_EQ(thread_ledger_page(db, &paged, &rows, &more), 0);
    MARY_ASSERT(more);
    int64_t last = 0;
    mc_json_int64(json_object_array_get_idx(rows, 0), "id", &last);
    json_object_put(rows);
    paged.before_id = last;
    MARY_ASSERT_EQ(thread_ledger_page(db, &paged, &rows, &more), 0);
    MARY_ASSERT_EQ(json_object_array_length(rows), 1);
    MARY_ASSERT(!more);
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(rows, 0), "kind"), "search");
    json_object_put(rows);
    thread_db_close(db);
}

MARY_TEST(the_ledger_keeps_only_the_newest_ten_thousand) {
    sqlite3 *db = NULL;
    MARY_ASSERT_EQ(thread_db_open(":memory:", &db), 0);
    thread_db_begin(db);
    thread_ledger_row row = { .kind = "embed", .source = "sewnd" };
    for (int i = 0; i < THREAD_LEDGER_KEEP + 300; i++) thread_ledger_record(db, &row);
    thread_db_commit(db);
    thread_ledger_roll(db);
    MARY_ASSERT_EQ(thread_ledger_count(db), THREAD_LEDGER_KEEP);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT MIN(id) FROM ledger", NULL, NULL), 301);
    thread_db_close(db);
}

int main(void) {
    MARY_RUN(rows_page_newest_first_with_their_documents_and_filters);
    MARY_RUN(the_ledger_keeps_only_the_newest_ten_thousand);
    MARY_TEST_MAIN_END();
}
