#include "test_support.h"
#include "common/log.h"

MARY_TEST(a_deposit_is_embedded_extracted_and_folded_into_the_graph) {
    thread_store *s = open_store(true);
    fake_extract_answer = "{\"entities\":[{\"name\":\"Paris\",\"kind\":\"place\"},{\"name\":\"France\",\"kind\":\"place\"}],"
                          "\"relationships\":[{\"subject\":\"Paris\",\"predicate\":\"capital of\",\"object\":\"France\"}]}";
    MARY_ASSERT_EQ(deposit_text(s, "mary", "conversation-mary", "mary-turn-1", "Paris is the capital of France.", NULL, NULL), 0);
    sqlite3 *db = thread_store_db(s);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM partitions WHERE embedding IS NULL", NULL, NULL), 1);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM jobs WHERE step = 'embed'", NULL, NULL), 1);
    MARY_ASSERT_EQ(thread_store_enrich_drain(s), 1);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM partitions WHERE embedding IS NULL", NULL, NULL), 0);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM jobs", NULL, NULL), 0);
    char *state = thread_db_text(db, "SELECT enrich_state FROM documents WHERE id = 'mary-turn-1'", NULL, NULL);
    MARY_ASSERT_STR(state, "done");
    free(state);
    MARY_ASSERT_EQ(fake_extract_calls, 1);
    /* the extracted graph replaced the keyword concepts */
    char paris[THREAD_GRAPH_ID_MAX];
    thread_entity_id("place", "Paris", paris);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM entities WHERE id = ?", paris, NULL), 1);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM entities WHERE kind = 'concept'", NULL, NULL), 0);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM relationships WHERE predicate = 'capital of' AND embedding IS NOT NULL", NULL, NULL), 1);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM predicates WHERE embedding IS NOT NULL", NULL, NULL), 1);
    /* partition ids hash the embedding and the document id */
    char *pid = thread_db_text(db, "SELECT id FROM partitions WHERE document_id = 'mary-turn-1'", NULL, NULL);
    MARY_ASSERT(pid && strlen(pid) >= 64);
    free(pid);
    /* embed, extract, and the graph fold each left a ledger row */
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM ledger WHERE kind = 'embed'", NULL, NULL), 1);
    MARY_ASSERT(thread_db_int(db, "SELECT COUNT(*) FROM ledger WHERE kind = 'extract'", NULL, NULL) >= 1);
    /* the same text again keeps its embeddings: the partition is not embedded again (the edge
     * extraction recreates is, since the re-deposit detached it) */
    MARY_ASSERT_EQ(deposit_text(s, "mary", "conversation-mary", "mary-turn-1", "Paris is the capital of France.", NULL, NULL), 0);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM partitions WHERE embedding IS NULL", NULL, NULL), 0);
    thread_store_enrich_drain(s);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM ledger WHERE kind = 'embed'", NULL, NULL), 1);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM relationships WHERE predicate = 'capital of' AND embedding IS NOT NULL", NULL, NULL), 1);
    close_store(s);
}

MARY_TEST(a_failing_embedder_backs_off_and_a_missing_extractor_keeps_the_keywords) {
    thread_store *s = open_store(true);
    sqlite3 *db = thread_store_db(s);
    fake_embed_rc = -EAGAIN;
    MARY_ASSERT_EQ(deposit_text(s, "mary", "g", "d1", "watering the plants on Tuesday", NULL, NULL), 0);
    MARY_ASSERT_EQ(thread_store_enrich_drain(s), 1);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT attempts FROM jobs WHERE document_id = 'd1'", NULL, NULL), 1);
    int64_t next = thread_db_int(db, "SELECT next_ms FROM jobs WHERE document_id = 'd1'", NULL, NULL);
    MARY_ASSERT(next > 0);
    char *err = thread_db_text(db, "SELECT last_error FROM jobs WHERE document_id = 'd1'", NULL, NULL);
    MARY_ASSERT_STR(err, "scripted failure");
    free(err);
    MARY_ASSERT_EQ(thread_store_enrich_drain(s), 0);                /* not due again yet */
    /* make it due, fail with -EIO: the delay doubles with each attempt */
    thread_db_exec(db, "UPDATE jobs SET next_ms = 0");
    fake_embed_rc = -EIO;
    int64_t before = thread_db_int(db, "SELECT next_ms FROM jobs WHERE document_id = 'd1'", NULL, NULL);
    (void)before;
    MARY_ASSERT_EQ(thread_store_enrich_drain(s), 1);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT attempts FROM jobs WHERE document_id = 'd1'", NULL, NULL), 2);
    /* recovery: the extractor is away, so the keyword entities stand and the job still completes */
    fake_embed_rc = 0;
    fake_extract_answer = NULL;
    thread_db_exec(db, "UPDATE jobs SET next_ms = 0");
    MARY_ASSERT_EQ(thread_store_enrich_drain(s), 1);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM jobs", NULL, NULL), 0);
    char *state = thread_db_text(db, "SELECT enrich_state FROM documents WHERE id = 'd1'", NULL, NULL);
    MARY_ASSERT_STR(state, "done");
    free(state);
    MARY_ASSERT(thread_db_int(db, "SELECT COUNT(*) FROM entities WHERE kind = 'concept'", NULL, NULL) >= 2);   /* watering, plants, tuesday */
    /* a re-deposit while a job is pending replaces the job: the old revision never writes */
    fake_extract_answer = "{\"entities\":[{\"name\":\"Plants\",\"kind\":\"concept\"}],\"relationships\":[]}";
    MARY_ASSERT_EQ(deposit_text(s, "mary", "g", "d1", "a wholly different note", NULL, NULL), 0);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT revision FROM documents WHERE id = 'd1'", NULL, NULL), 2);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT revision FROM jobs WHERE document_id = 'd1'", NULL, NULL), 2);
    thread_db_exec(db, "UPDATE jobs SET revision = 1");             /* pretend the queued job is stale */
    MARY_ASSERT_EQ(thread_store_enrich_drain(s), 1);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM jobs", NULL, NULL), 0);     /* dropped, not applied */
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM partitions WHERE embedding IS NULL", NULL, NULL), 1);
    close_store(s);
}

MARY_TEST(re_extract_queues_the_document_again) {
    thread_store *s = open_store(true);
    fake_extract_answer = "{\"entities\":[{\"name\":\"Note\",\"kind\":\"work\"}],\"relationships\":[]}";
    MARY_ASSERT_EQ(deposit_text(s, "mary", "g", "d", "a note about notes", "Old", "concept"), 0);
    thread_store_enrich_drain(s);
    MARY_ASSERT_EQ(thread_db_int(thread_store_db(s), "SELECT COUNT(*) FROM entities WHERE name = 'Old'", NULL, NULL), 1);
    MARY_ASSERT_EQ(thread_store_reextract(s, "mary", "d", "desktop"), 0);
    MARY_ASSERT_EQ(thread_store_reextract(s, "guest", "d", "desktop"), -ENOENT);
    MARY_ASSERT_EQ(thread_store_enrich_drain(s), 1);
    MARY_ASSERT_EQ(thread_db_int(thread_store_db(s), "SELECT COUNT(*) FROM entities WHERE name = 'Old'", NULL, NULL), 0);
    MARY_ASSERT_EQ(thread_db_int(thread_store_db(s), "SELECT COUNT(*) FROM entities WHERE name = 'Note'", NULL, NULL), 1);
    close_store(s);
}

int main(void) {
    if (getenv("MARY_LOG_DEBUG")) mc_log_set_threshold(MC_LOG_DEBUG);
    MARY_RUN(a_deposit_is_embedded_extracted_and_folded_into_the_graph);
    MARY_RUN(a_failing_embedder_backs_off_and_a_missing_extractor_keeps_the_keywords);
    MARY_RUN(re_extract_queues_the_document_again);
    MARY_TEST_MAIN_END();
}
