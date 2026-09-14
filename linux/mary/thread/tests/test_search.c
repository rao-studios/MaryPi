#include "test_support.h"

MARY_TEST(search_finds_the_closest_chunks_within_the_lanes_and_groups_asked) {
    thread_store *s = open_store(true);
    MARY_ASSERT_EQ(deposit_text(s, "mary", "memory-mary", "mary-turn-1", "What is the capital of France? Paris is the capital of France.", "France", "place"), 0);
    MARY_ASSERT_EQ(deposit_text(s, "mary", "memory-mary", "mary-turn-2", "Remind me to water the plants on Tuesday.", "plants", "concept"), 0);
    MARY_ASSERT_EQ(deposit_text(s, "mary", "files-mary", "file-1", "France travel notes: Paris, the Louvre, the Seine.", "France", "place"), 0);
    MARY_ASSERT_EQ(deposit_text(s, "guest", "files-guest", "file-g", "Paris in the spring, a guest's notes on France.", "France", "place"), 0);
    MARY_ASSERT(thread_store_enrich_drain(s) >= 3);
    struct json_object *r = NULL;
    thread_search_request q = { .query_text = "capital of France", .top_k = 10, .request_id = "req-1", .source = "maryd" };
    MARY_ASSERT_EQ(thread_store_search(s, "mary", &q, &r), 0);
    struct json_object *results = mc_json_array(r, "results");
    MARY_ASSERT(json_object_array_length(results) >= 2);
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(results, 0), "document_id"), "mary-turn-1");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(results, 0), "lane"), "personal");
    for (size_t i = 0; i < json_object_array_length(results); i++)
        MARY_ASSERT(strcmp(mc_json_string(json_object_array_get_idx(results, i), "document_id"), "file-g") != 0);   /* never another owner's */
    /* the graph matched "france" and narrowed; the trace says so */
    struct json_object *trace = mc_json_object(r, "trace");
    MARY_ASSERT(trace != NULL);
    if (trace) MARY_ASSERT(json_object_array_length(mc_json_array(trace, "matched_entity_ids")) >= 1);
    int64_t ledger_id = 0;
    MARY_ASSERT(mc_json_int64(r, "ledger_id", &ledger_id) && ledger_id > 0);
    json_object_put(r);
    /* the ledger row lists what came back, in rank order, under the request id */
    MARY_ASSERT_EQ(thread_db_int(thread_store_db(s), "SELECT COUNT(*) FROM ledger WHERE kind = 'search' AND request_id = 'req-1'", NULL, NULL), 1);
    MARY_ASSERT(thread_db_int(thread_store_db(s), "SELECT COUNT(*) FROM ledger_documents WHERE rank = 0 AND document_id = 'mary-turn-1'", NULL, NULL) == 1);
    MARY_ASSERT_EQ(thread_db_int(thread_store_db(s), "SELECT retrieval_count FROM document_stats WHERE document_id = 'mary-turn-1'", NULL, NULL), 1);
    /* a lane constrains, the expansion included: nothing here is behavioral */
    const char *lanes[] = { "behavioral" };
    thread_search_request lane_q = { .query_text = "capital of France", .lanes = lanes, .n_lanes = 1, .top_k = 10 };
    MARY_ASSERT_EQ(thread_store_search(s, "mary", &lane_q, &r), 0);
    results = mc_json_array(r, "results");
    MARY_ASSERT_EQ(json_object_array_length(results), 0);
    json_object_put(r);
    const char *personal[] = { "personal" };
    lane_q.lanes = personal;
    MARY_ASSERT_EQ(thread_store_search(s, "mary", &lane_q, &r), 0);
    results = mc_json_array(r, "results");
    MARY_ASSERT(json_object_array_length(results) >= 1);
    for (size_t i = 0; i < json_object_array_length(results); i++) MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(results, i), "lane"), "personal");
    json_object_put(r);
    /* a group constrains */
    const char *groups[] = { "memory-mary" };
    thread_search_request group_q = { .query_text = "water the plants", .group_ids = groups, .n_groups = 1, .top_k = 1 };
    MARY_ASSERT_EQ(thread_store_search(s, "mary", &group_q, &r), 0);
    results = mc_json_array(r, "results");
    MARY_ASSERT_EQ(json_object_array_length(results), 1);
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(results, 0), "document_id"), "mary-turn-2");
    json_object_put(r);
    /* a query vector given in place of text is used as it is; the cache serves repeats */
    int calls = fake_embed_calls;
    MARY_ASSERT_EQ(thread_store_search(s, "mary", &group_q, &r), 0);
    json_object_put(r);
    MARY_ASSERT_EQ(fake_embed_calls, calls);
    float v[TEST_DIM];
    bag_of_words("plants Tuesday", v);
    thread_search_request vec_q = { .query_embedding = v, .dim = TEST_DIM, .top_k = 2 };
    MARY_ASSERT_EQ(thread_store_search(s, "mary", &vec_q, &r), 0);
    MARY_ASSERT_EQ(fake_embed_calls, calls);
    json_object_put(r);
    close_store(s);
}

MARY_TEST(without_an_embedder_search_says_so) {
    thread_store *s = open_store(false);
    MARY_ASSERT_EQ(deposit_text(s, "mary", "g", "d", "some text", NULL, NULL), 0);
    struct json_object *r = NULL;
    thread_search_request q = { .query_text = "text" };
    MARY_ASSERT_EQ(thread_store_search(s, "mary", &q, &r), -ENOSYS);
    MARY_ASSERT(r == NULL);
    close_store(s);
}

MARY_TEST(the_graph_query_browses_seeds_and_walks) {
    thread_store *s = open_store(true);
    const char *texts[] = { "Ada Lovelace worked with Charles Babbage on the Analytical Engine." };
    thread_graph_payload p = { 0 };
    thread_payload_add_entity(&p, "Ada Lovelace", "person");
    thread_payload_add_entity(&p, "Charles Babbage", "person");
    thread_payload_add_entity(&p, "Analytical Engine", "work");
    thread_payload_add_relation(&p, "Ada Lovelace", "worked with", "Charles Babbage");
    thread_payload_add_relation(&p, "Charles Babbage", "designed", "Analytical Engine");
    thread_deposit d = { .document_id = "ada", .group_id = "files-mary", .texts = texts, .n_texts = 1, .graph = &p };
    MARY_ASSERT_EQ(thread_store_deposit(s, "mary", &d, NULL, NULL), 0);
    thread_payload_free(&p);
    thread_store_enrich_drain(s);
    struct json_object *r = NULL;
    thread_graph_request browse = { .limit = 10, .include_documents = true };
    MARY_ASSERT_EQ(thread_store_graph_query(s, "mary", &browse, &r), 0);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(r, "entities")), 3);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(r, "relationships")), 2);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(r, "documents")), 1);
    int64_t n = 0;
    MARY_ASSERT(mc_json_int64(r, "entity_count", &n) && n == 3);
    json_object_put(r);
    thread_graph_request seeded = { .entity = "lovelace", .hops = 1, .limit = 10 };
    MARY_ASSERT_EQ(thread_store_graph_query(s, "mary", &seeded, &r), 0);
    struct json_object *ents = mc_json_array(r, "entities");
    MARY_ASSERT_EQ(json_object_array_length(ents), 2);               /* Ada and Babbage, one hop */
    double score = 0;
    MARY_ASSERT(mc_json_double(json_object_array_get_idx(ents, 0), "score", &score) && score == 1.0);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(r, "relationships")), 1);
    json_object_put(r);
    thread_graph_request deeper = { .entity = "lovelace", .hops = 2, .limit = 10 };
    MARY_ASSERT_EQ(thread_store_graph_query(s, "mary", &deeper, &r), 0);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(r, "entities")), 3);
    json_object_put(r);
    /* repairs go through the store, with a ledger row */
    char babbage[THREAD_GRAPH_ID_MAX];
    thread_entity_id("person", "Charles Babbage", babbage);
    MARY_ASSERT_EQ(thread_store_graph_mutate(s, "mary", "rename", babbage, "C. Babbage", NULL, NULL, "desktop", &r), 0);
    MARY_ASSERT(mc_json_string(r, "surviving_id") && strcmp(mc_json_string(r, "surviving_id"), babbage) != 0);
    json_object_put(r);
    MARY_ASSERT_EQ(thread_db_int(thread_store_db(s), "SELECT COUNT(*) FROM ledger WHERE kind = 'mutation'", NULL, NULL), 1);
    MARY_ASSERT_EQ(thread_store_graph_mutate(s, "mary", "rename", "nope", "x", NULL, NULL, NULL, &r), -ENOENT);
    MARY_ASSERT_EQ(thread_store_graph_mutate(s, "mary", "explode", "x", NULL, NULL, NULL, NULL, &r), -EINVAL);
    close_store(s);
}

int main(void) {
    MARY_RUN(search_finds_the_closest_chunks_within_the_lanes_and_groups_asked);
    MARY_RUN(without_an_embedder_search_says_so);
    MARY_RUN(the_graph_query_browses_seeds_and_walks);
    MARY_TEST_MAIN_END();
}
