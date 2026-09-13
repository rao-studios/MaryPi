#include <errno.h>
#include <stdlib.h>

#include "common/json.h"
#include "mary_test.h"
#include "thread/db.h"
#include "thread/graph.h"
#include "thread/text.h"

static sqlite3 *fresh(void) {
    sqlite3 *db = NULL;
    MARY_ASSERT_EQ(thread_db_open(":memory:", &db), 0);
    /* documents must exist for the provenance foreign keys */
    thread_db_exec(db, "INSERT INTO groups(id, owner, created_at) VALUES('g', 'mary', 0);"
                       "INSERT INTO documents(id, owner, group_id, created_at) VALUES('a', 'mary', 'g', 0), ('b', 'mary', 'g', 0), ('c', 'mary', 'g', 0);");
    return db;
}

static thread_graph_payload ada_and_babbage(void) {
    thread_graph_payload p = { 0 };
    thread_payload_add_entity(&p, "Ada Lovelace", "person");
    thread_payload_add_entity(&p, "Charles Babbage", "person");
    thread_payload_add_entity(&p, "Analytical Engine", "work");
    thread_payload_add_relation(&p, "Ada Lovelace", "worked with", "Charles Babbage");
    thread_payload_add_relation(&p, "Charles Babbage", "designed", "Analytical Engine");
    return p;
}

MARY_TEST(ids_are_content_addressed_like_the_swift) {
    char a[THREAD_GRAPH_ID_MAX], b[THREAD_GRAPH_ID_MAX], c[THREAD_GRAPH_ID_MAX];
    thread_entity_id("Person", "  Ada   Lovelace ", a);
    mf_numeric_hash_text("person|ada lovelace", b);
    MARY_ASSERT_STR(a, b);
    thread_entity_id("", "Ada Lovelace", c);
    mf_numeric_hash_text("concept|ada lovelace", b);
    MARY_ASSERT_STR(c, b);
    thread_relationship_id("1", " Worked With ", "2", a);
    mf_numeric_hash_text("1|worked with|2", b);
    MARY_ASSERT_STR(a, b);
    thread_predicate_id("Worked With", a);
    mf_numeric_hash_text("worked with", b);
    MARY_ASSERT_STR(a, b);
    char s[256];
    thread_predicate_embed_string("part of", s, sizeof s);
    MARY_ASSERT_STR(s, "Relationship predicate: part of");
    thread_relationship_embed_string("Person", "Ada", "worked with", "person", "Babbage", s, sizeof s);
    MARY_ASSERT_STR(s, "person: Ada worked with person: Babbage");
}

MARY_TEST(upsert_merges_mentions_and_weights_across_documents) {
    sqlite3 *db = fresh();
    thread_graph_payload p = ada_and_babbage();
    MARY_ASSERT_EQ(thread_graph_upsert(db, "a", &p, 4), 0);
    MARY_ASSERT_EQ(thread_graph_upsert(db, "b", &p, 4), 0);
    thread_payload_free(&p);
    char ada[THREAD_GRAPH_ID_MAX];
    thread_entity_id("person", "Ada Lovelace", ada);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT mention_count FROM entities WHERE id = ?", ada, NULL), 2);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM entity_documents WHERE entity_id = ?", ada, NULL), 2);
    MARY_ASSERT_EQ(thread_graph_entity_count(db), 3);
    MARY_ASSERT_EQ(thread_graph_relationship_count(db), 2);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT weight FROM relationships WHERE predicate = 'worked with'", NULL, NULL), 2);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT relationship_count FROM predicates WHERE name = 'designed'", NULL, NULL), 2);
    /* the name keeps its casing; tokens are normalized */
    char *name = thread_db_text(db, "SELECT name FROM entities WHERE id = ?", ada, NULL);
    MARY_ASSERT_STR(name, "Ada Lovelace");
    free(name);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM entity_tokens WHERE entity_id = ?", ada, NULL), 2);

    /* an unknown endpoint, an empty predicate and a self-loop are dropped */
    thread_graph_payload q = { 0 };
    thread_payload_add_entity(&q, "Ada Lovelace", "person");
    thread_payload_add_relation(&q, "Ada Lovelace", "knew", "Nobody");
    thread_payload_add_relation(&q, "Ada Lovelace", " ", "Charles Babbage");
    thread_payload_add_relation(&q, "Ada Lovelace", "is", "Ada Lovelace");
    MARY_ASSERT_EQ(thread_graph_upsert(db, "c", &q, 4), 0);
    thread_payload_free(&q);
    MARY_ASSERT_EQ(thread_graph_relationship_count(db), 2);
    /* a relationship may name an entity that only the graph knows */
    thread_graph_payload r = { 0 };
    thread_payload_add_entity(&r, "Babbage's mill", "work");
    thread_payload_add_relation(&r, "Babbage's mill", "part of", "Analytical Engine");
    MARY_ASSERT_EQ(thread_graph_upsert(db, "c", &r, 4), 0);
    thread_payload_free(&r);
    MARY_ASSERT_EQ(thread_graph_relationship_count(db), 3);
    thread_db_close(db);
}

MARY_TEST(detach_decrements_and_deletes_what_no_document_holds) {
    sqlite3 *db = fresh();
    thread_graph_payload p = ada_and_babbage();
    thread_graph_upsert(db, "a", &p, 4);
    thread_graph_upsert(db, "b", &p, 4);
    thread_payload_free(&p);
    MARY_ASSERT_EQ(thread_graph_detach(db, "a"), 0);
    char ada[THREAD_GRAPH_ID_MAX];
    thread_entity_id("person", "Ada Lovelace", ada);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT mention_count FROM entities WHERE id = ?", ada, NULL), 1);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT weight FROM relationships WHERE predicate = 'worked with'", NULL, NULL), 1);
    MARY_ASSERT_EQ(thread_graph_relationship_count(db), 2);
    MARY_ASSERT_EQ(thread_graph_detach(db, "b"), 0);
    MARY_ASSERT_EQ(thread_graph_entity_count(db), 0);
    MARY_ASSERT_EQ(thread_graph_relationship_count(db), 0);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM predicates", NULL, NULL), 0);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM entity_tokens", NULL, NULL), 0);
    thread_db_close(db);
}

MARY_TEST(rename_merge_and_kind_re_key_the_entity_and_its_edges) {
    sqlite3 *db = fresh();
    thread_graph_payload p = ada_and_babbage();
    thread_graph_upsert(db, "a", &p, 4);
    thread_payload_free(&p);
    thread_graph_payload q = { 0 };
    thread_payload_add_entity(&q, "Lady Lovelace", "person");
    thread_payload_add_entity(&q, "Charles Babbage", "person");
    thread_payload_add_relation(&q, "Lady Lovelace", "worked with", "Charles Babbage");
    thread_graph_upsert(db, "b", &q, 4);
    thread_payload_free(&q);
    MARY_ASSERT_EQ(thread_graph_entity_count(db), 4);
    char lady[THREAD_GRAPH_ID_MAX], ada[THREAD_GRAPH_ID_MAX];
    thread_entity_id("person", "Lady Lovelace", lady);
    thread_entity_id("person", "Ada Lovelace", ada);
    /* renaming Lady Lovelace to Ada Lovelace merges: provenance {a, b}, mentions summed, one edge of weight 2 */
    thread_mutation m;
    MARY_ASSERT_EQ(thread_graph_rename(db, lady, "Ada Lovelace", &m), 0);
    MARY_ASSERT_STR(m.surviving_id, ada);
    MARY_ASSERT_EQ(m.affected.n, 2);
    thread_mutation_free(&m);
    MARY_ASSERT_EQ(thread_graph_entity_count(db), 3);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT mention_count FROM entities WHERE id = ?", ada, NULL), 2);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM relationships WHERE predicate = 'worked with'", NULL, NULL), 1);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT weight FROM relationships WHERE predicate = 'worked with'", NULL, NULL), 2);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM relationship_documents WHERE relationship_id = (SELECT id FROM relationships WHERE predicate = 'worked with')", NULL, NULL), 2);
    MARY_ASSERT(!thread_graph_has_entity(db, lady));
    /* a kind change re-keys */
    MARY_ASSERT_EQ(thread_graph_set_kind(db, ada, "other", &m), 0);
    char other[THREAD_GRAPH_ID_MAX];
    thread_entity_id("other", "Ada Lovelace", other);
    MARY_ASSERT_STR(m.surviving_id, other);
    thread_mutation_free(&m);
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM relationships WHERE subject_id = ?", other, NULL), 1);
    /* merging Babbage into the engine collapses their edge into a self-loop that disappears */
    char babbage[THREAD_GRAPH_ID_MAX], engine[THREAD_GRAPH_ID_MAX];
    thread_entity_id("person", "Charles Babbage", babbage);
    thread_entity_id("work", "Analytical Engine", engine);
    MARY_ASSERT_EQ(thread_graph_merge(db, babbage, engine, &m), 0);
    MARY_ASSERT_STR(m.surviving_id, engine);
    thread_mutation_free(&m);
    MARY_ASSERT_EQ(thread_graph_entity_count(db), 2);
    MARY_ASSERT_EQ(thread_graph_relationship_count(db), 1);         /* only "worked with", now Ada → Engine */
    MARY_ASSERT_EQ(thread_db_int(db, "SELECT COUNT(*) FROM predicates", NULL, NULL), 1);
    /* a rename to blank is a no-op; an unknown id is ENOENT */
    MARY_ASSERT_EQ(thread_graph_rename(db, other, "   ", &m), 0);
    MARY_ASSERT_STR(m.surviving_id, other);
    thread_mutation_free(&m);
    MARY_ASSERT_EQ(thread_graph_rename(db, "nope", "x", &m), -ENOENT);
    /* deleting an entity removes its edges and names the documents to rewrite */
    MARY_ASSERT_EQ(thread_graph_delete_entity(db, other, &m), 0);
    MARY_ASSERT_EQ(m.affected.n, 2);
    thread_mutation_free(&m);
    MARY_ASSERT_EQ(thread_graph_relationship_count(db), 0);
    thread_db_close(db);
}

MARY_TEST(matching_and_the_neighbourhood) {
    sqlite3 *db = fresh();
    thread_graph_payload p = ada_and_babbage();
    /* give the edges embeddings: a 4-dim toy space */
    float worked[4] = { 1, 0, 0, 0 }, designed[4] = { 0, 1, 0, 0 };
    p.relationships[0].embedding = malloc(sizeof worked);
    memcpy(p.relationships[0].embedding, worked, sizeof worked);
    p.relationships[0].predicate_embedding = malloc(sizeof worked);
    memcpy(p.relationships[0].predicate_embedding, worked, sizeof worked);
    p.relationships[1].embedding = malloc(sizeof designed);
    memcpy(p.relationships[1].embedding, designed, sizeof designed);
    thread_graph_upsert(db, "a", &p, 4);
    thread_payload_free(&p);

    thread_match *m = NULL;
    size_t n = 0;
    MARY_ASSERT_EQ(thread_graph_match_entities(db, "tell me about lovelace and the engine", NULL, 0, 8, &m, &n), 0);
    MARY_ASSERT_EQ(n, 2);
    free(m);
    const char *kinds[] = { "work" };
    MARY_ASSERT_EQ(thread_graph_match_entities(db, "lovelace engine", kinds, 1, 8, &m, &n), 0);
    MARY_ASSERT_EQ(n, 1);
    char engine[THREAD_GRAPH_ID_MAX];
    thread_entity_id("work", "Analytical Engine", engine);
    if (n) MARY_ASSERT_STR(m[0].id, engine);
    free(m);

    float q[4] = { 0.9f, 0.1f, 0, 0 };
    MARY_ASSERT_EQ(thread_graph_match_relationships(db, q, 4, NULL, 12, &m, &n), 0);
    MARY_ASSERT_EQ(n, 1);                        /* designed scores 0.1 < 0.15 */
    if (n) MARY_ASSERT_NEAR(m[0].score, 0.9, 1e-5);
    free(m);
    thread_ids seeds = { 0 };
    thread_ids_add(&seeds, engine);
    MARY_ASSERT_EQ(thread_graph_match_relationships(db, q, 4, &seeds, 12, &m, &n), 0);
    MARY_ASSERT_EQ(n, 2);                        /* the seeded edge is floored at the threshold */
    free(m);

    thread_ids entities = { 0 }, edges = { 0 };
    MARY_ASSERT_EQ(thread_graph_neighborhood(db, &seeds, 1, &entities, &edges), 0);
    MARY_ASSERT_EQ(entities.n, 2);
    MARY_ASSERT_EQ(edges.n, 1);
    thread_ids_free(&entities);
    thread_ids_free(&edges);
    MARY_ASSERT_EQ(thread_graph_neighborhood(db, &seeds, 2, &entities, &edges), 0);
    MARY_ASSERT_EQ(entities.n, 3);
    MARY_ASSERT_EQ(edges.n, 2);
    thread_ids docs = { 0 };
    MARY_ASSERT_EQ(thread_graph_documents_of_entities(db, &entities, &docs), 0);
    MARY_ASSERT_EQ(docs.n, 1);
    MARY_ASSERT_EQ(thread_graph_edge_weight_for(db, "a", &edges), 2);
    thread_ids_free(&docs);
    thread_ids_free(&entities);
    thread_ids_free(&edges);
    thread_ids_free(&seeds);

    thread_ids top = { 0 };
    MARY_ASSERT_EQ(thread_graph_top_entities(db, NULL, 0, 2, &top), 0);
    MARY_ASSERT_EQ(top.n, 2);
    thread_ids among = { 0 };
    MARY_ASSERT_EQ(thread_graph_edges_among(db, &top, &among), 0);
    thread_ids_free(&top);
    thread_ids_free(&among);
    thread_db_close(db);
}

MARY_TEST(the_parser_accepts_wrapped_json_and_keeps_only_known_endpoints) {
    thread_policy policy;
    thread_policy_default(&policy);
    const char *answer = "Sure! Here is the graph:\n```json\n{\"entities\":[{\"name\":\" Ada Lovelace \",\"kind\":\"Person\"},"
                         "{\"name\":\"Ada Lovelace\",\"kind\":\"person\"},{\"name\":\"Mathematics\",\"kind\":\"discipline\"},{\"name\":\"\",\"kind\":\"person\"}],"
                         "\"relationships\":[{\"subject\":\"Ada Lovelace\",\"predicate\":\" Studied \",\"object\":\"Mathematics\"},"
                         "{\"subject\":\"Ada Lovelace\",\"predicate\":\"knew\",\"object\":\"Nobody\"},"
                         "{\"subject\":\"Ada Lovelace\",\"predicate\":\"studied\",\"object\":\"Mathematics\"}]}\n```";
    thread_graph_payload p;
    MARY_ASSERT_EQ(thread_graph_payload_parse(answer, strlen(answer), &policy, &p), 0);
    MARY_ASSERT_EQ(p.n_entities, 2);
    MARY_ASSERT_STR(p.entities[0].name, "Ada Lovelace");
    MARY_ASSERT_STR(p.entities[0].kind, "person");
    MARY_ASSERT_STR(p.entities[1].kind, "concept");          /* out of the ontology → concept */
    MARY_ASSERT_EQ(p.n_relationships, 1);
    MARY_ASSERT_STR(p.relationships[0].predicate, "studied");
    thread_payload_free(&p);
    MARY_ASSERT_EQ(thread_graph_payload_parse("no json here", 12, &policy, &p), -EINVAL);
    /* the prompt names the ontology and the caps */
    char *prompt = thread_policy_system_prompt(&policy);
    MARY_ASSERT(strstr(prompt, "person (A human individual.)") != NULL);
    MARY_ASSERT(strstr(prompt, "at most 12 entities and 15 relationships") != NULL);
    MARY_ASSERT(strstr(prompt, "person|organization|place") != NULL);
    free(prompt);
    /* a policy round-trips through JSON, with aliases applied to predicates */
    const char *json = "{\"kinds\":[{\"name\":\"file\",\"description\":\"a file\"}],\"max_entities\":3,\"predicate_aliases\":{\"works for\":\"employed by\"},"
                       "\"co_mention\":{\"enabled\":true,\"predicate\":\"appears with\"},\"hub_degree_cap\":2}";
    thread_policy q;
    MARY_ASSERT_EQ(thread_policy_from_json(json, strlen(json), &q), 0);
    MARY_ASSERT_EQ(q.n_kinds, 1);
    MARY_ASSERT_EQ(q.max_entities, 3);
    MARY_ASSERT(q.co_mention);
    char *pred = thread_policy_normalize_predicate(&q, " Works For ");
    MARY_ASSERT_STR(pred, "employed by");
    free(pred);
    struct json_object *o = thread_policy_json(&q);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(o, "kinds")), 1);
    json_object_put(o);
    /* co-mention edges between every pair not explicitly linked */
    thread_graph_payload r = { 0 };
    thread_payload_add_entity(&r, "A", "file");
    thread_payload_add_entity(&r, "B", "file");
    thread_payload_add_entity(&r, "C", "file");
    thread_payload_add_relation(&r, "A", "works for", "B");
    MARY_ASSERT_EQ(thread_graph_apply_policy(NULL, &r, &q), 0);
    MARY_ASSERT_EQ(r.n_relationships, 3);              /* A-B explicit, plus auto A-C and B-C */
    MARY_ASSERT_STR(r.relationships[0].predicate, "employed by");
    MARY_ASSERT_STR(r.relationships[1].predicate, "auto:appears with");
    thread_payload_free(&r);
    thread_policy_free(&q);
    thread_policy_free(&policy);
}

int main(void) {
    MARY_RUN(ids_are_content_addressed_like_the_swift);
    MARY_RUN(upsert_merges_mentions_and_weights_across_documents);
    MARY_RUN(detach_decrements_and_deletes_what_no_document_holds);
    MARY_RUN(rename_merge_and_kind_re_key_the_entity_and_its_edges);
    MARY_RUN(matching_and_the_neighbourhood);
    MARY_RUN(the_parser_accepts_wrapped_json_and_keeps_only_known_endpoints);
    MARY_TEST_MAIN_END();
}
