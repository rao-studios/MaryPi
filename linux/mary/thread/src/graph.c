#include "thread/graph.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/json.h"
#include "common/log.h"
#include "thread/db.h"
#include "thread/text.h"

/* MARK: - ids */

int thread_ids_add(thread_ids *ids, const char *id) {
    if (thread_ids_has(ids, id)) return 0;
    if (ids->n == ids->cap) {
        size_t cap = ids->cap ? ids->cap * 2 : 16;
        char **v = realloc(ids->v, cap * sizeof *v);
        if (!v) return -ENOMEM;
        ids->v = v;
        ids->cap = cap;
    }
    ids->v[ids->n] = strdup(id);
    if (!ids->v[ids->n]) return -ENOMEM;
    ids->n++;
    return 1;
}

bool thread_ids_has(const thread_ids *ids, const char *id) {
    for (size_t i = 0; i < ids->n; i++) if (strcmp(ids->v[i], id) == 0) return true;
    return false;
}

void thread_ids_free(thread_ids *ids) {
    for (size_t i = 0; i < ids->n; i++) free(ids->v[i]);
    free(ids->v);
    memset(ids, 0, sizeof *ids);
}

/* MARK: - payloads */

int thread_payload_add_entity(thread_graph_payload *p, const char *name, const char *kind) {
    thread_entity_in *v = realloc(p->entities, (p->n_entities + 1) * sizeof *v);
    if (!v) return -ENOMEM;
    p->entities = v;
    v[p->n_entities].name = strdup(name ? name : "");
    v[p->n_entities].kind = strdup(kind && *kind ? kind : "concept");
    p->n_entities++;
    return 0;
}

int thread_payload_add_relation(thread_graph_payload *p, const char *subject, const char *predicate, const char *object) {
    thread_relation_in *v = realloc(p->relationships, (p->n_relationships + 1) * sizeof *v);
    if (!v) return -ENOMEM;
    p->relationships = v;
    thread_relation_in *r = &v[p->n_relationships++];
    memset(r, 0, sizeof *r);
    r->subject = strdup(subject ? subject : "");
    r->predicate = strdup(predicate ? predicate : "");
    r->object = strdup(object ? object : "");
    return 0;
}

void thread_payload_free(thread_graph_payload *p) {
    for (size_t i = 0; i < p->n_entities; i++) {
        free(p->entities[i].name);
        free(p->entities[i].kind);
    }
    for (size_t i = 0; i < p->n_relationships; i++) {
        free(p->relationships[i].subject);
        free(p->relationships[i].predicate);
        free(p->relationships[i].object);
        free(p->relationships[i].embedding);
        free(p->relationships[i].predicate_embedding);
    }
    free(p->entities);
    free(p->relationships);
    memset(p, 0, sizeof *p);
}

struct json_object *thread_payload_json(const thread_graph_payload *p) {
    struct json_object *o = json_object_new_object(), *ents = json_object_new_array(), *rels = json_object_new_array();
    for (size_t i = 0; i < p->n_entities; i++) {
        struct json_object *e = json_object_new_object();
        json_object_object_add(e, "name", json_object_new_string(p->entities[i].name));
        json_object_object_add(e, "kind", json_object_new_string(p->entities[i].kind));
        json_object_array_add(ents, e);
    }
    for (size_t i = 0; i < p->n_relationships; i++) {
        struct json_object *r = json_object_new_object();
        json_object_object_add(r, "subject", json_object_new_string(p->relationships[i].subject));
        json_object_object_add(r, "predicate", json_object_new_string(p->relationships[i].predicate));
        json_object_object_add(r, "object", json_object_new_string(p->relationships[i].object));
        json_object_array_add(rels, r);
    }
    json_object_object_add(o, "entities", ents);
    json_object_object_add(o, "relationships", rels);
    return o;
}

int thread_payload_from_json(struct json_object *o, thread_graph_payload *out) {
    memset(out, 0, sizeof *out);
    struct json_object *ents = mc_json_array(o, "entities"), *rels = mc_json_array(o, "relationships");
    for (size_t i = 0; ents && i < json_object_array_length(ents); i++) {
        struct json_object *e = json_object_array_get_idx(ents, i);
        const char *name = mc_json_string(e, "name");
        if (name) thread_payload_add_entity(out, name, mc_json_string(e, "kind"));
    }
    for (size_t i = 0; rels && i < json_object_array_length(rels); i++) {
        struct json_object *r = json_object_array_get_idx(rels, i);
        const char *s = mc_json_string(r, "subject"), *p = mc_json_string(r, "predicate"), *ob = mc_json_string(r, "object");
        if (s && p && ob) thread_payload_add_relation(out, s, p, ob);
    }
    return 0;
}

/* MARK: - policy */

static const thread_policy_kind DEFAULT_KINDS[] = {
    { "person", "A human individual." },
    { "organization", "A company, institution, or group." },
    { "place", "A geographic location." },
    { "event", "A happening at a point or span in time." },
    { "work", "A created artifact: book, paper, product, system." },
    { "concept", "An abstract idea, topic, or term." },
    { "other", "Anything that fits no other kind." },
};

void thread_policy_default(thread_policy *p) {
    memset(p, 0, sizeof *p);
    p->n_kinds = sizeof DEFAULT_KINDS / sizeof DEFAULT_KINDS[0];
    memcpy(p->kinds, DEFAULT_KINDS, sizeof DEFAULT_KINDS);
    p->max_entities = 12;
    p->max_relationships = 15;
    p->co_mention = false;
    snprintf(p->co_mention_predicate, sizeof p->co_mention_predicate, "appears with");
    p->skip_explicitly_linked = true;
    p->hub_degree_cap = 24;
}

void thread_policy_free(thread_policy *p) {
    if (p->aliases) json_object_put(p->aliases);
    free(p->prompt_template);
    p->aliases = NULL;
    p->prompt_template = NULL;
}

int thread_policy_from_json(const char *json, size_t len, thread_policy *out) {
    thread_policy_default(out);
    struct json_object *o = mc_json_parse(json, len);
    if (!o) return -EINVAL;
    struct json_object *kinds = mc_json_array(o, "kinds");
    if (kinds && json_object_array_length(kinds)) {
        out->n_kinds = 0;
        for (size_t i = 0; i < json_object_array_length(kinds) && out->n_kinds < 16; i++) {
            struct json_object *k = json_object_array_get_idx(kinds, i);
            const char *name = mc_json_string(k, "name"), *description = mc_json_string(k, "description");
            if (!name || !*name) continue;
            snprintf(out->kinds[out->n_kinds].name, sizeof out->kinds[0].name, "%s", name);
            snprintf(out->kinds[out->n_kinds].description, sizeof out->kinds[0].description, "%s", description ? description : "");
            out->n_kinds++;
        }
        if (!out->n_kinds) {
            out->n_kinds = sizeof DEFAULT_KINDS / sizeof DEFAULT_KINDS[0];
            memcpy(out->kinds, DEFAULT_KINDS, sizeof DEFAULT_KINDS);
        }
    }
    int64_t v;
    if (mc_json_int64(o, "max_entities", &v) && v > 0) out->max_entities = (int)v;
    if (mc_json_int64(o, "max_relationships", &v) && v > 0) out->max_relationships = (int)v;
    if (mc_json_int64(o, "hub_degree_cap", &v)) out->hub_degree_cap = v > 0 ? (int)v : 0;
    const char *tpl = mc_json_string(o, "prompt_template");
    if (tpl && *tpl) out->prompt_template = strdup(tpl);
    struct json_object *aliases = mc_json_object(o, "predicate_aliases");
    if (aliases) out->aliases = json_object_get(aliases);
    struct json_object *co = mc_json_object(o, "co_mention");
    if (co) {
        bool enabled = true;
        mc_json_bool(co, "enabled", &enabled);
        out->co_mention = enabled;
        const char *pred = mc_json_string(co, "predicate");
        if (pred && *pred) snprintf(out->co_mention_predicate, sizeof out->co_mention_predicate, "%s", pred);
        bool skip = true;
        mc_json_bool(co, "skipExplicitlyLinked", &skip);
        out->skip_explicitly_linked = skip;
    }
    json_object_put(o);
    return 0;
}

struct json_object *thread_policy_json(const thread_policy *p) {
    struct json_object *o = json_object_new_object(), *kinds = json_object_new_array();
    for (size_t i = 0; i < p->n_kinds; i++) {
        struct json_object *k = json_object_new_object();
        json_object_object_add(k, "name", json_object_new_string(p->kinds[i].name));
        json_object_object_add(k, "description", json_object_new_string(p->kinds[i].description));
        json_object_array_add(kinds, k);
    }
    json_object_object_add(o, "kinds", kinds);
    if (p->prompt_template) json_object_object_add(o, "prompt_template", json_object_new_string(p->prompt_template));
    json_object_object_add(o, "predicate_aliases", p->aliases ? json_object_get(p->aliases) : json_object_new_object());
    json_object_object_add(o, "max_entities", json_object_new_int(p->max_entities));
    json_object_object_add(o, "max_relationships", json_object_new_int(p->max_relationships));
    struct json_object *co = json_object_new_object();
    json_object_object_add(co, "enabled", json_object_new_boolean(p->co_mention));
    json_object_object_add(co, "predicate", json_object_new_string(p->co_mention_predicate));
    json_object_object_add(co, "skipExplicitlyLinked", json_object_new_boolean(p->skip_explicitly_linked));
    json_object_object_add(o, "co_mention", co);
    if (p->hub_degree_cap > 0) json_object_object_add(o, "hub_degree_cap", json_object_new_int(p->hub_degree_cap));
    return o;
}

static const char DEFAULT_PROMPT[] =
    "You extract a knowledge graph from text. Respond with ONLY a JSON object \xE2\x80\x94 no prose, no code fences:\n"
    "{\"entities\":[{\"name\":\"...\",\"kind\":\"{{kind_names}}\"}],\"relationships\":[{\"subject\":\"...\",\"predicate\":\"...\",\"object\":\"...\"}]}\n"
    "Allowed kinds: {{kinds}}.\n"
    "Rules: at most {{max_entities}} entities and {{max_relationships}} relationships; subject and object MUST "
    "exactly match a name from entities; predicates are short lowercase verb phrases "
    "(\"founded\",\"works at\",\"part of\"); keep original name casing; no duplicates.";

/* Replaces every `key` in `text` with `value`. Heap. */
static char *replace_all(const char *text, const char *key, const char *value) {
    size_t klen = strlen(key), vlen = strlen(value), count = 0;
    for (const char *p = strstr(text, key); p; p = strstr(p + klen, key)) count++;
    char *out = malloc(strlen(text) + count * (vlen > klen ? vlen - klen : 0) + 1);
    if (!out) return NULL;
    char *w = out;
    const char *p = text;
    for (const char *hit = strstr(p, key); hit; hit = strstr(p, key)) {
        memcpy(w, p, (size_t)(hit - p));
        w += hit - p;
        memcpy(w, value, vlen);
        w += vlen;
        p = hit + klen;
    }
    strcpy(w, p);
    return out;
}

char *thread_policy_system_prompt(const thread_policy *p) {
    char list[2048] = "", names[512] = "";
    for (size_t i = 0; i < p->n_kinds; i++) {
        size_t l = strlen(list), n = strlen(names);
        snprintf(list + l, sizeof list - l, "%s%s (%s)", i ? ", " : "", p->kinds[i].name, p->kinds[i].description);
        snprintf(names + n, sizeof names - n, "%s%s", i ? "|" : "", p->kinds[i].name);
    }
    char max_e[16], max_r[16];
    snprintf(max_e, sizeof max_e, "%d", p->max_entities);
    snprintf(max_r, sizeof max_r, "%d", p->max_relationships);
    char *a = replace_all(p->prompt_template ? p->prompt_template : DEFAULT_PROMPT, "{{kinds}}", list);
    char *b = a ? replace_all(a, "{{kind_names}}", names) : NULL;
    char *c = b ? replace_all(b, "{{max_entities}}", max_e) : NULL;
    char *d = c ? replace_all(c, "{{max_relationships}}", max_r) : NULL;
    free(a);
    free(b);
    free(c);
    return d;
}

char *thread_policy_normalize_predicate(const thread_policy *p, const char *predicate) {
    char *key = thread_lower_trim(predicate ? predicate : "");
    if (!key) return NULL;
    const char *canonical = p && p->aliases ? mc_json_string(p->aliases, key) : NULL;
    if (canonical) {
        char *out = strdup(canonical);
        free(key);
        return out;
    }
    return key;
}

/* MARK: - identity */

void thread_entity_id(const char *kind, const char *name, char out[THREAD_GRAPH_ID_MAX]) {
    char k[THREAD_KIND_MAX];
    thread_normalize_kind(kind, k);
    char *n = thread_normalize_name(name ? name : "");
    size_t len = strlen(k) + 1 + (n ? strlen(n) : 0);
    char *key = malloc(len + 1);
    if (key) {
        snprintf(key, len + 1, "%s|%s", k, n ? n : "");
        mf_numeric_hash_text(key, out);
    } else {
        out[0] = 0;
    }
    free(key);
    free(n);
}

void thread_relationship_id(const char *subject_id, const char *predicate, const char *object_id, char out[THREAD_GRAPH_ID_MAX]) {
    char *p = thread_lower_trim(predicate ? predicate : "");
    size_t len = strlen(subject_id) + 1 + (p ? strlen(p) : 0) + 1 + strlen(object_id);
    char *key = malloc(len + 1);
    if (key) {
        snprintf(key, len + 1, "%s|%s|%s", subject_id, p ? p : "", object_id);
        mf_numeric_hash_text(key, out);
    } else {
        out[0] = 0;
    }
    free(key);
    free(p);
}

void thread_predicate_id(const char *predicate, char out[THREAD_GRAPH_ID_MAX]) {
    char *p = thread_lower_trim(predicate ? predicate : "");
    mf_numeric_hash_text(p ? p : "", out);
    free(p);
}

void thread_predicate_embed_string(const char *predicate, char *out, size_t cap) {
    snprintf(out, cap, "Relationship predicate: %s", predicate);
}

void thread_relationship_embed_string(const char *subject_kind, const char *subject, const char *predicate,
                                      const char *object_kind, const char *object, char *out, size_t cap) {
    char sk[THREAD_KIND_MAX], ok[THREAD_KIND_MAX];
    thread_normalize_kind(subject_kind, sk);
    thread_normalize_kind(object_kind, ok);
    snprintf(out, cap, "%s: %s %s %s: %s", sk, subject, predicate, ok, object);
}

float thread_dot(const float *a, const float *b, size_t dim) {
    float s = 0;
    for (size_t i = 0; i < dim; i++) s += a[i] * b[i];
    return s;
}

/* MARK: - the parser (GraphExtractionParser.parse) */

int thread_graph_payload_parse(const char *response, size_t len, const thread_policy *policy, thread_graph_payload *out) {
    memset(out, 0, sizeof *out);
    thread_policy defaults;
    if (!policy) {
        thread_policy_default(&defaults);
        policy = &defaults;
    }
    const char *first = memchr(response, '{', len);
    const char *last = NULL;
    for (size_t i = len; i > 0; i--) if (response[i - 1] == '}') { last = response + i - 1; break; }
    if (!first || !last || first >= last) return -EINVAL;
    struct json_object *raw = mc_json_parse(first, (size_t)(last - first + 1));
    if (!raw) return -EINVAL;
    int entity_cap = policy->max_entities < 12 ? policy->max_entities : 12;
    int relationship_cap = policy->max_relationships < 15 ? policy->max_relationships : 15;

    thread_ids seen_keys = { 0 }, names = { 0 };
    struct json_object *ents = mc_json_array(raw, "entities");
    for (size_t i = 0; ents && i < json_object_array_length(ents); i++) {
        struct json_object *e = json_object_array_get_idx(ents, i);
        const char *raw_name = mc_json_string(e, "name");
        if (!raw_name) continue;
        char *name = thread_lower_trim(raw_name);        /* only for the emptiness check */
        bool empty = !name || !*name;
        free(name);
        if (empty) continue;
        /* trimmed, original casing */
        const unsigned char *s = (const unsigned char *)raw_name;
        size_t ws;
        while ((ws = thread_space_len(s))) s += ws;
        size_t n = strlen((const char *)s);
        while (n) {
            size_t back = 0;
            for (size_t k = 1; k <= 3 && k <= n; k++) if (thread_space_len(s + n - k) == k) back = k;
            if (!back) break;
            n -= back;
        }
        char trimmed[n + 1];
        memcpy(trimmed, s, n);
        trimmed[n] = 0;
        char kind[THREAD_KIND_MAX];
        const char *raw_kind = mc_json_string(e, "kind");
        thread_normalize_kind(raw_kind ? raw_kind : "concept", kind);
        bool allowed = false, has_concept = false;
        for (size_t k = 0; k < policy->n_kinds; k++) {
            char pk[THREAD_KIND_MAX];
            thread_normalize_kind(policy->kinds[k].name, pk);
            if (strcmp(pk, kind) == 0) allowed = true;
            if (strcmp(pk, "concept") == 0) has_concept = true;
        }
        if (policy->n_kinds && !allowed) {
            if (has_concept) snprintf(kind, sizeof kind, "concept");
            else thread_normalize_kind(policy->kinds[0].name, kind);
        }
        char *norm = thread_normalize_name(trimmed);
        char key[THREAD_KIND_MAX + 2 + strlen(norm)];
        snprintf(key, sizeof key, "%s|%s", kind, norm);
        if (thread_ids_add(&seen_keys, key) == 1) {
            thread_payload_add_entity(out, trimmed, kind);
            thread_ids_add(&names, norm);
        }
        free(norm);
        if ((int)out->n_entities >= entity_cap) break;
    }
    thread_ids seen_rels = { 0 };
    struct json_object *rels = mc_json_array(raw, "relationships");
    for (size_t i = 0; rels && i < json_object_array_length(rels); i++) {
        struct json_object *r = json_object_array_get_idx(rels, i);
        const char *s = mc_json_string(r, "subject"), *o = mc_json_string(r, "object"), *p = mc_json_string(r, "predicate");
        if (!s || !o || !p) continue;
        char *predicate = thread_policy_normalize_predicate(policy, p);
        char *sn = thread_normalize_name(s), *on = thread_normalize_name(o);
        if (predicate && *predicate && sn && on && thread_ids_has(&names, sn) && thread_ids_has(&names, on)) {
            char key[strlen(sn) + strlen(predicate) + strlen(on) + 3];
            snprintf(key, sizeof key, "%s|%s|%s", sn, predicate, on);
            if (thread_ids_add(&seen_rels, key) == 1) {
                /* subject/object trimmed as the entity was */
                char *st = thread_lower_trim(s), *ot = thread_lower_trim(o);
                (void)st;
                (void)ot;
                free(st);
                free(ot);
                thread_payload_add_relation(out, s, predicate, o);
            }
        }
        free(predicate);
        free(sn);
        free(on);
        if ((int)out->n_relationships >= relationship_cap) break;
    }
    thread_ids_free(&seen_keys);
    thread_ids_free(&names);
    thread_ids_free(&seen_rels);
    json_object_put(raw);
    if (policy == &defaults) thread_policy_free(&defaults);
    return 0;
}

int thread_graph_entity_degree(sqlite3 *db, const char *id) {
    return (int)thread_db_int(db, "SELECT COUNT(*) FROM relationships WHERE subject_id = ?1 OR object_id = ?1", id, NULL);
}

bool thread_graph_has_entity(sqlite3 *db, const char *id) {
    return thread_db_int(db, "SELECT COUNT(*) FROM entities WHERE id = ?", id, NULL) > 0;
}

int thread_graph_apply_policy(sqlite3 *db, thread_graph_payload *p, const thread_policy *policy) {
    if (policy->aliases) {
        for (size_t i = 0; i < p->n_relationships; i++) {
            if (strncmp(p->relationships[i].predicate, THREAD_AUTO_PREDICATE_PREFIX, strlen(THREAD_AUTO_PREDICATE_PREFIX)) == 0) continue;
            char *canonical = thread_policy_normalize_predicate(policy, p->relationships[i].predicate);
            if (canonical) {
                free(p->relationships[i].predicate);
                p->relationships[i].predicate = canonical;
            }
        }
    }
    if (!policy->co_mention || p->n_entities < 2) return 0;
    char auto_predicate[80];
    snprintf(auto_predicate, sizeof auto_predicate, "%s%s", THREAD_AUTO_PREDICATE_PREFIX, policy->co_mention_predicate);
    thread_ids explicit = { 0 };
    for (size_t i = 0; i < p->n_relationships; i++) {
        char *a = thread_normalize_name(p->relationships[i].subject), *b = thread_normalize_name(p->relationships[i].object);
        if (a && b) {
            char key[strlen(a) + strlen(b) + 2];
            if (strcmp(a, b) <= 0) snprintf(key, sizeof key, "%s|%s", a, b);
            else snprintf(key, sizeof key, "%s|%s", b, a);
            thread_ids_add(&explicit, key);
        }
        free(a);
        free(b);
    }
    size_t n = p->n_entities;
    int cap = policy->hub_degree_cap > 0 ? policy->hub_degree_cap : 0;
    for (size_t a = 0; a + 1 < n; a++) {
        for (size_t b = a + 1; b < n; b++) {
            char *an = thread_normalize_name(p->entities[a].name), *bn = thread_normalize_name(p->entities[b].name);
            char key[(an ? strlen(an) : 0) + (bn ? strlen(bn) : 0) + 2];
            if (an && bn) {
                if (strcmp(an, bn) <= 0) snprintf(key, sizeof key, "%s|%s", an, bn);
                else snprintf(key, sizeof key, "%s|%s", bn, an);
            } else key[0] = 0;
            bool skip = policy->skip_explicitly_linked && thread_ids_has(&explicit, key);
            free(an);
            free(bn);
            if (skip) continue;
            if (cap && db) {
                char ida[THREAD_GRAPH_ID_MAX], idb[THREAD_GRAPH_ID_MAX];
                thread_entity_id(p->entities[a].kind, p->entities[a].name, ida);
                thread_entity_id(p->entities[b].kind, p->entities[b].name, idb);
                if (thread_graph_entity_degree(db, ida) >= cap || thread_graph_entity_degree(db, idb) >= cap) continue;
            }
            thread_payload_add_relation(p, p->entities[a].name, auto_predicate, p->entities[b].name);
        }
    }
    thread_ids_free(&explicit);
    return 0;
}

/* MARK: - SQL helpers */

static int step_done(sqlite3 *db, sqlite3_stmt *stmt, const char *what) {
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        mc_log(MC_LOG_ERROR, "graph: %s: %s", what, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -EIO;
    }
    sqlite3_finalize(stmt);
    return 0;
}

static int prepare(sqlite3 *db, const char *sql, sqlite3_stmt **stmt) {
    if (sqlite3_prepare_v2(db, sql, -1, stmt, NULL) != SQLITE_OK) {
        mc_log(MC_LOG_ERROR, "graph: prepare: %s: %s", sql, sqlite3_errmsg(db));
        return -EIO;
    }
    return 0;
}

static int recount_predicates(sqlite3 *db) {
    int rc = thread_db_exec(db, "UPDATE predicates SET relationship_count = (SELECT COALESCE(SUM(weight), 0) FROM relationships WHERE predicate_id = predicates.id)");
    if (rc == 0) rc = thread_db_exec(db, "DELETE FROM predicates WHERE relationship_count <= 0");
    return rc;
}

/* Replaces an entity's name tokens. */
static int write_tokens(sqlite3 *db, const char *id, const char *name_norm) {
    sqlite3_stmt *stmt = NULL;
    int rc = prepare(db, "DELETE FROM entity_tokens WHERE entity_id = ?", &stmt);
    if (rc) return rc;
    thread_db_bind_text(stmt, 1, id);
    rc = step_done(db, stmt, "tokens");
    const char *p = name_norm;
    while (rc == 0 && *p) {
        while (*p == ' ') p++;
        const char *start = p;
        while (*p && *p != ' ') p++;
        if (p == start) continue;
        char token[(size_t)(p - start) + 1];
        memcpy(token, start, (size_t)(p - start));
        token[p - start] = 0;
        rc = prepare(db, "INSERT OR IGNORE INTO entity_tokens(token, entity_id) VALUES(?, ?)", &stmt);
        if (rc) return rc;
        thread_db_bind_text(stmt, 1, token);
        thread_db_bind_text(stmt, 2, id);
        rc = step_done(db, stmt, "tokens");
    }
    return rc;
}

/* The entity whose normalized name is `norm`, into out; false when none. */
static bool entity_by_name(sqlite3 *db, const char *norm, char out[THREAD_GRAPH_ID_MAX]) {
    char *id = thread_db_text(db, "SELECT id FROM entities WHERE name_norm = ? ORDER BY mention_count DESC, id LIMIT 1", norm, NULL);
    if (!id) return false;
    snprintf(out, THREAD_GRAPH_ID_MAX, "%s", id);
    free(id);
    return true;
}

static char *trim_copy(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    size_t ws;
    while ((ws = thread_space_len(p))) p += ws;
    size_t n = strlen((const char *)p);
    while (n) {
        size_t back = 0;
        for (size_t k = 1; k <= 3 && k <= n; k++) if (thread_space_len(p + n - k) == k) back = k;
        if (!back) break;
        n -= back;
    }
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, p, n);
    out[n] = 0;
    return out;
}

/* MARK: - upsert */

typedef struct name_map {
    thread_ids norms;
    thread_ids ids;
} name_map;

int thread_graph_upsert(sqlite3 *db, const char *document_id, const thread_graph_payload *p, size_t dim) {
    name_map map = { { 0 }, { 0 } };
    int rc = 0;
    for (size_t i = 0; rc == 0 && i < p->n_entities; i++) {
        char *trimmed = trim_copy(p->entities[i].name);
        if (!trimmed) return -ENOMEM;
        if (!*trimmed) {
            free(trimmed);
            continue;
        }
        char kind[THREAD_KIND_MAX], id[THREAD_GRAPH_ID_MAX];
        thread_normalize_kind(p->entities[i].kind, kind);
        thread_entity_id(kind, trimmed, id);
        char *norm = thread_normalize_name(trimmed);
        sqlite3_stmt *stmt = NULL;
        rc = prepare(db, "INSERT INTO entities(id, name, name_norm, kind, mention_count) VALUES(?, ?, ?, ?, 1) "
                         "ON CONFLICT(id) DO UPDATE SET mention_count = mention_count + 1", &stmt);
        if (rc == 0) {
            thread_db_bind_text(stmt, 1, id);
            thread_db_bind_text(stmt, 2, trimmed);
            thread_db_bind_text(stmt, 3, norm);
            thread_db_bind_text(stmt, 4, kind);
            rc = step_done(db, stmt, "entity");
        }
        if (rc == 0) rc = prepare(db, "INSERT OR IGNORE INTO entity_documents(entity_id, document_id, position) VALUES(?, ?, ?)", &stmt);
        if (rc == 0) {
            thread_db_bind_text(stmt, 1, id);
            thread_db_bind_text(stmt, 2, document_id);
            sqlite3_bind_int(stmt, 3, (int)i);
            rc = step_done(db, stmt, "provenance");
        }
        if (rc == 0) rc = write_tokens(db, id, norm ? norm : "");
        if (rc == 0 && norm) {
            /* later names win, as the Swift dictionary does */
            size_t at = map.norms.n;
            for (size_t k = 0; k < map.norms.n; k++) if (strcmp(map.norms.v[k], norm) == 0) at = k;
            if (at == map.norms.n) {
                thread_ids_add(&map.norms, norm);
                thread_ids_add(&map.ids, id);
                /* thread_ids_add is unique on ids; keep the arrays aligned */
                if (map.ids.n != map.norms.n) {
                    free(map.ids.v[map.ids.n - 1]);
                    map.ids.n--;
                    map.ids.v[map.ids.n] = strdup(id);
                    map.ids.n++;
                }
            } else {
                free(map.ids.v[at]);
                map.ids.v[at] = strdup(id);
            }
        }
        free(norm);
        free(trimmed);
    }
    for (size_t i = 0; rc == 0 && i < p->n_relationships; i++) {
        const thread_relation_in *r = &p->relationships[i];
        char *sn = thread_normalize_name(r->subject), *on = thread_normalize_name(r->object), *predicate = thread_lower_trim(r->predicate);
        char sid[THREAD_GRAPH_ID_MAX] = "", oid[THREAD_GRAPH_ID_MAX] = "";
        bool have_s = false, have_o = false;
        for (size_t k = 0; k < map.norms.n; k++) {
            if (sn && strcmp(map.norms.v[k], sn) == 0) { snprintf(sid, sizeof sid, "%s", map.ids.v[k]); have_s = true; }
            if (on && strcmp(map.norms.v[k], on) == 0) { snprintf(oid, sizeof oid, "%s", map.ids.v[k]); have_o = true; }
        }
        if (!have_s && sn) have_s = entity_by_name(db, sn, sid);
        if (!have_o && on) have_o = entity_by_name(db, on, oid);
        if (predicate && *predicate && have_s && have_o && strcmp(sid, oid) != 0) {
            char rid[THREAD_GRAPH_ID_MAX], pid[THREAD_GRAPH_ID_MAX];
            thread_relationship_id(sid, predicate, oid, rid);
            thread_predicate_id(predicate, pid);
            sqlite3_stmt *stmt = NULL;
            rc = prepare(db, "INSERT INTO relationships(id, subject_id, predicate, predicate_id, object_id, weight, embedding) VALUES(?, ?, ?, ?, ?, 1, ?) "
                             "ON CONFLICT(id) DO UPDATE SET weight = weight + 1, embedding = COALESCE(embedding, excluded.embedding)", &stmt);
            if (rc == 0) {
                thread_db_bind_text(stmt, 1, rid);
                thread_db_bind_text(stmt, 2, sid);
                thread_db_bind_text(stmt, 3, predicate);
                thread_db_bind_text(stmt, 4, pid);
                thread_db_bind_text(stmt, 5, oid);
                thread_db_bind_floats(stmt, 6, r->embedding, dim);
                rc = step_done(db, stmt, "relationship");
            }
            if (rc == 0) rc = prepare(db, "INSERT OR IGNORE INTO relationship_documents(relationship_id, document_id) VALUES(?, ?)", &stmt);
            if (rc == 0) {
                thread_db_bind_text(stmt, 1, rid);
                thread_db_bind_text(stmt, 2, document_id);
                rc = step_done(db, stmt, "provenance");
            }
            if (rc == 0) rc = prepare(db, "INSERT INTO predicates(id, name, embedding, relationship_count) VALUES(?, ?, ?, 1) "
                                          "ON CONFLICT(id) DO UPDATE SET relationship_count = relationship_count + 1, embedding = COALESCE(embedding, excluded.embedding)", &stmt);
            if (rc == 0) {
                thread_db_bind_text(stmt, 1, pid);
                thread_db_bind_text(stmt, 2, predicate);
                thread_db_bind_floats(stmt, 3, r->predicate_embedding, dim);
                rc = step_done(db, stmt, "predicate");
            }
        }
        free(sn);
        free(on);
        free(predicate);
    }
    thread_ids_free(&map.norms);
    thread_ids_free(&map.ids);
    return rc;
}

/* MARK: - detach */

/* The ids one column of `sql` yields for `arg`. */
static int collect(sqlite3 *db, const char *sql, const char *arg, thread_ids *out) {
    sqlite3_stmt *stmt = NULL;
    int rc = prepare(db, sql, &stmt);
    if (rc) return rc;
    if (arg) thread_db_bind_text(stmt, 1, arg);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *id = sqlite3_column_text(stmt, 0);
        if (id) thread_ids_add(out, (const char *)id);
    }
    sqlite3_finalize(stmt);
    return 0;
}

static int exec1(sqlite3 *db, const char *sql, const char *a1, const char *a2) {
    sqlite3_stmt *stmt = NULL;
    int rc = prepare(db, sql, &stmt);
    if (rc) return rc;
    if (a1) thread_db_bind_text(stmt, 1, a1);
    if (a2) thread_db_bind_text(stmt, 2, a2);
    return step_done(db, stmt, sql);
}

int thread_graph_detach(sqlite3 *db, const char *document_id) {
    thread_ids entities = { 0 }, edges = { 0 };
    int rc = collect(db, "SELECT entity_id FROM entity_documents WHERE document_id = ?", document_id, &entities);
    for (size_t i = 0; rc == 0 && i < entities.n; i++)
        rc = collect(db, "SELECT id FROM relationships WHERE subject_id = ?1 OR object_id = ?1", entities.v[i], &edges);
    for (size_t i = 0; rc == 0 && i < edges.n; i++) {
        rc = exec1(db, "DELETE FROM relationship_documents WHERE relationship_id = ? AND document_id = ?", edges.v[i], document_id);
        if (rc == 0) rc = exec1(db, "UPDATE relationships SET weight = MAX(0, weight - 1) WHERE id = ?", edges.v[i], NULL);
        if (rc == 0 && thread_db_int(db, "SELECT COUNT(*) FROM relationship_documents WHERE relationship_id = ?", edges.v[i], NULL) == 0)
            rc = exec1(db, "DELETE FROM relationships WHERE id = ?", edges.v[i], NULL);
    }
    for (size_t i = 0; rc == 0 && i < entities.n; i++) {
        rc = exec1(db, "DELETE FROM entity_documents WHERE entity_id = ? AND document_id = ?", entities.v[i], document_id);
        if (rc == 0) rc = exec1(db, "UPDATE entities SET mention_count = MAX(0, mention_count - 1) WHERE id = ?", entities.v[i], NULL);
        if (rc == 0 && thread_db_int(db, "SELECT COUNT(*) FROM entity_documents WHERE entity_id = ?", entities.v[i], NULL) == 0)
            rc = exec1(db, "DELETE FROM entities WHERE id = ?", entities.v[i], NULL);   /* cascades its edges */
    }
    if (rc == 0) rc = recount_predicates(db);
    thread_ids_free(&entities);
    thread_ids_free(&edges);
    return rc;
}

/* MARK: - mutations (GraphStore+Mutations.swift) */

void thread_mutation_free(thread_mutation *m) { thread_ids_free(&m->affected); }

static int rekey(sqlite3 *db, const char *old_id, const char *new_kind, const char *new_name, thread_mutation *out) {
    memset(out, 0, sizeof *out);
    char *old_name = thread_db_text(db, "SELECT name FROM entities WHERE id = ?", old_id, NULL);
    if (!old_name) return -ENOENT;
    int64_t old_mentions = thread_db_int(db, "SELECT mention_count FROM entities WHERE id = ?", old_id, NULL);
    char kind[THREAD_KIND_MAX];
    thread_normalize_kind(new_kind, kind);
    char *trimmed = trim_copy(new_name);
    free(old_name);
    if (!trimmed) return -ENOMEM;
    if (!*trimmed) {
        free(trimmed);
        snprintf(out->surviving_id, sizeof out->surviving_id, "%s", old_id);
        return 0;
    }
    char new_id[THREAD_GRAPH_ID_MAX];
    thread_entity_id(kind, trimmed, new_id);
    char *norm = thread_normalize_name(trimmed);
    int rc = collect(db, "SELECT document_id FROM entity_documents WHERE entity_id = ?", old_id, &out->affected);
    if (rc == 0 && strcmp(new_id, old_id) == 0) {
        rc = exec1(db, "UPDATE entities SET name = ? WHERE id = ?", trimmed, old_id);
        if (rc == 0) rc = exec1(db, "UPDATE entities SET name_norm = ? WHERE id = ?", norm, old_id);
        if (rc == 0) rc = write_tokens(db, old_id, norm);
        thread_ids_free(&out->affected);
        snprintf(out->surviving_id, sizeof out->surviving_id, "%s", old_id);
        free(trimmed);
        free(norm);
        return rc;
    }
    bool exists = thread_graph_has_entity(db, new_id);
    if (rc == 0 && exists) {
        rc = collect(db, "SELECT document_id FROM entity_documents WHERE entity_id = ?", new_id, &out->affected);
        char mentions[32];
        snprintf(mentions, sizeof mentions, "%lld", (long long)old_mentions);
        if (rc == 0) rc = exec1(db, "UPDATE entities SET mention_count = mention_count + ? WHERE id = ?", mentions, new_id);
        if (rc == 0) rc = exec1(db, "INSERT OR IGNORE INTO entity_documents(entity_id, document_id, position) "
                                    "SELECT ?, document_id, position FROM entity_documents WHERE entity_id = ?", new_id, old_id);
    } else if (rc == 0) {
        sqlite3_stmt *stmt = NULL;
        rc = prepare(db, "INSERT INTO entities(id, name, name_norm, kind, mention_count) VALUES(?, ?, ?, ?, ?)", &stmt);
        if (rc == 0) {
            thread_db_bind_text(stmt, 1, new_id);
            thread_db_bind_text(stmt, 2, trimmed);
            thread_db_bind_text(stmt, 3, norm);
            thread_db_bind_text(stmt, 4, kind);
            sqlite3_bind_int64(stmt, 5, old_mentions);
            rc = step_done(db, stmt, "entity");
        }
        if (rc == 0) rc = exec1(db, "INSERT OR IGNORE INTO entity_documents(entity_id, document_id, position) "
                                    "SELECT ?, document_id, position FROM entity_documents WHERE entity_id = ?", new_id, old_id);
        if (rc == 0) rc = write_tokens(db, new_id, norm);
    }
    /* Incident edges re-key with the endpoint. */
    sqlite3_stmt *stmt = NULL;
    if (rc == 0) rc = prepare(db, "SELECT id, subject_id, predicate, predicate_id, object_id, weight FROM relationships WHERE subject_id = ?1 OR object_id = ?1", &stmt);
    if (rc == 0) {
        thread_db_bind_text(stmt, 1, old_id);
        struct edge { char id[THREAD_GRAPH_ID_MAX], s[THREAD_GRAPH_ID_MAX], pid[THREAD_GRAPH_ID_MAX], o[THREAD_GRAPH_ID_MAX]; char *predicate; int weight; } *edges = NULL;
        size_t n = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            struct edge *v = realloc(edges, (n + 1) * sizeof *v);
            if (!v) break;
            edges = v;
            snprintf(edges[n].id, sizeof edges[n].id, "%s", (const char *)sqlite3_column_text(stmt, 0));
            snprintf(edges[n].s, sizeof edges[n].s, "%s", (const char *)sqlite3_column_text(stmt, 1));
            edges[n].predicate = strdup((const char *)sqlite3_column_text(stmt, 2));
            snprintf(edges[n].pid, sizeof edges[n].pid, "%s", (const char *)sqlite3_column_text(stmt, 3));
            snprintf(edges[n].o, sizeof edges[n].o, "%s", (const char *)sqlite3_column_text(stmt, 4));
            edges[n].weight = sqlite3_column_int(stmt, 5);
            n++;
        }
        sqlite3_finalize(stmt);
        for (size_t i = 0; rc == 0 && i < n; i++) {
            const char *s = strcmp(edges[i].s, old_id) == 0 ? new_id : edges[i].s;
            const char *o = strcmp(edges[i].o, old_id) == 0 ? new_id : edges[i].o;
            if (strcmp(s, o) == 0) {
                rc = exec1(db, "DELETE FROM relationships WHERE id = ?", edges[i].id, NULL);
                continue;
            }
            char new_rid[THREAD_GRAPH_ID_MAX];
            thread_relationship_id(s, edges[i].predicate, o, new_rid);
            char weight[32];
            snprintf(weight, sizeof weight, "%d", edges[i].weight);
            if (thread_db_int(db, "SELECT COUNT(*) FROM relationships WHERE id = ?", new_rid, NULL) > 0) {
                rc = exec1(db, "UPDATE relationships SET weight = weight + ? WHERE id = ?", weight, new_rid);
            } else {
                sqlite3_stmt *ins = NULL;
                rc = prepare(db, "INSERT INTO relationships(id, subject_id, predicate, predicate_id, object_id, weight, embedding) VALUES(?, ?, ?, ?, ?, ?, NULL)", &ins);
                if (rc == 0) {
                    thread_db_bind_text(ins, 1, new_rid);
                    thread_db_bind_text(ins, 2, s);
                    thread_db_bind_text(ins, 3, edges[i].predicate);
                    thread_db_bind_text(ins, 4, edges[i].pid);
                    thread_db_bind_text(ins, 5, o);
                    sqlite3_bind_int(ins, 6, edges[i].weight);
                    rc = step_done(db, ins, "relationship");
                }
            }
            if (rc == 0) rc = exec1(db, "INSERT OR IGNORE INTO relationship_documents(relationship_id, document_id) "
                                        "SELECT ?, document_id FROM relationship_documents WHERE relationship_id = ?", new_rid, edges[i].id);
            if (rc == 0) rc = exec1(db, "DELETE FROM relationships WHERE id = ?", edges[i].id, NULL);
        }
        for (size_t i = 0; i < n; i++) free(edges[i].predicate);
        free(edges);
    }
    if (rc == 0) rc = exec1(db, "DELETE FROM entities WHERE id = ?", old_id, NULL);
    if (rc == 0) rc = recount_predicates(db);
    snprintf(out->surviving_id, sizeof out->surviving_id, "%s", new_id);
    free(trimmed);
    free(norm);
    return rc;
}

int thread_graph_rename(sqlite3 *db, const char *id, const char *new_name, thread_mutation *out) {
    char *kind = thread_db_text(db, "SELECT kind FROM entities WHERE id = ?", id, NULL);
    if (!kind) {
        memset(out, 0, sizeof *out);
        return -ENOENT;
    }
    int rc = rekey(db, id, kind, new_name, out);
    free(kind);
    return rc;
}

int thread_graph_set_kind(sqlite3 *db, const char *id, const char *kind, thread_mutation *out) {
    char *name = thread_db_text(db, "SELECT name FROM entities WHERE id = ?", id, NULL);
    if (!name) {
        memset(out, 0, sizeof *out);
        return -ENOENT;
    }
    int rc = rekey(db, id, kind, name, out);
    free(name);
    return rc;
}

int thread_graph_merge(sqlite3 *db, const char *from, const char *into, thread_mutation *out) {
    memset(out, 0, sizeof *out);
    if (strcmp(from, into) == 0 || !thread_graph_has_entity(db, from)) return -EINVAL;
    char *kind = thread_db_text(db, "SELECT kind FROM entities WHERE id = ?", into, NULL);
    char *name = thread_db_text(db, "SELECT name FROM entities WHERE id = ?", into, NULL);
    int rc = kind && name ? rekey(db, from, kind, name, out) : -ENOENT;
    free(kind);
    free(name);
    return rc;
}

int thread_graph_delete_entity(sqlite3 *db, const char *id, thread_mutation *out) {
    memset(out, 0, sizeof *out);
    if (!thread_graph_has_entity(db, id)) return -ENOENT;
    int rc = collect(db, "SELECT document_id FROM entity_documents WHERE entity_id = ?", id, &out->affected);
    if (rc == 0) rc = exec1(db, "DELETE FROM entities WHERE id = ?", id, NULL);
    if (rc == 0) rc = recount_predicates(db);
    return rc;
}

int thread_graph_delete_relationship(sqlite3 *db, const char *id) {
    int rc = exec1(db, "DELETE FROM relationships WHERE id = ?", id, NULL);
    return rc ? rc : recount_predicates(db);
}

/* MARK: - readers */

static bool kind_allowed(const char *kind, const char *const *kinds, size_t nkinds) {
    if (!nkinds) return true;
    for (size_t i = 0; i < nkinds; i++) {
        char k[THREAD_KIND_MAX];
        thread_normalize_kind(kinds[i], k);
        if (strcmp(k, kind) == 0) return true;
    }
    return false;
}

int thread_graph_match_entities(sqlite3 *db, const char *name_query, const char *const *kinds, size_t nkinds, int limit,
                                thread_match **out, size_t *n) {
    *out = NULL;
    *n = 0;
    if (!name_query) return 0;
    char *norm = thread_normalize_name(name_query);
    if (!norm) return -ENOMEM;
    if (limit <= 0) limit = THREAD_ENTITY_MATCH_LIMIT;
    thread_ids ids = { 0 };
    const char *p = norm;
    int rc = 0;
    while (rc == 0 && *p) {
        while (*p == ' ') p++;
        const char *start = p;
        while (*p && *p != ' ') p++;
        if (p == start) continue;
        char token[(size_t)(p - start) + 1];
        memcpy(token, start, (size_t)(p - start));
        token[p - start] = 0;
        rc = collect(db, "SELECT e.id FROM entity_tokens t JOIN entities e ON e.id = t.entity_id WHERE t.token = ? ORDER BY e.mention_count DESC, e.id", token, &ids);
    }
    free(norm);
    thread_match *matches = ids.n ? calloc(ids.n, sizeof *matches) : NULL;
    size_t count = 0;
    for (size_t i = 0; matches && i < ids.n; i++) {
        char *kind = thread_db_text(db, "SELECT kind FROM entities WHERE id = ?", ids.v[i], NULL);
        bool ok = kind && kind_allowed(kind, kinds, nkinds);
        free(kind);
        if (!ok) continue;
        snprintf(matches[count].id, sizeof matches[count].id, "%s", ids.v[i]);
        matches[count].score = 1.0f;
        count++;
    }
    thread_ids_free(&ids);
    /* stable by mention count then id: re-sort the kept ones */
    if (count > (size_t)limit) count = (size_t)limit;
    *out = matches;
    *n = count;
    return rc;
}

static int compare_matches(const void *a, const void *b) {
    const thread_match *x = a, *y = b;
    if (x->score != y->score) return x->score > y->score ? -1 : 1;
    if (x->weight != y->weight) return x->weight > y->weight ? -1 : 1;
    return strcmp(x->id, y->id);
}

int thread_graph_match_relationships(sqlite3 *db, const float *q, size_t dim, const thread_ids *seeds, int limit,
                                     thread_match **out, size_t *n) {
    *out = NULL;
    *n = 0;
    if (!q || !dim) return 0;
    if (limit <= 0) limit = THREAD_RELATIONSHIP_MATCH_LIMIT;
    /* predicate scores */
    thread_match *pred = NULL;
    size_t npred = 0;
    sqlite3_stmt *stmt = NULL;
    int rc = prepare(db, "SELECT id, embedding FROM predicates WHERE embedding IS NOT NULL", &stmt);
    if (rc) return rc;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        float *v = thread_db_column_floats(stmt, 1, dim);
        if (!v) continue;
        float score = thread_dot(v, q, dim);
        free(v);
        if (score < THREAD_PREDICATE_MATCH_THRESHOLD) continue;
        thread_match *grown = realloc(pred, (npred + 1) * sizeof *grown);
        if (!grown) break;
        pred = grown;
        memset(&pred[npred], 0, sizeof pred[npred]);
        snprintf(pred[npred].id, sizeof pred[npred].id, "%s", (const char *)sqlite3_column_text(stmt, 0));
        pred[npred].score = score;
        npred++;
    }
    sqlite3_finalize(stmt);
    thread_match *matches = NULL;
    size_t count = 0;
    rc = prepare(db, "SELECT id, subject_id, object_id, predicate_id, weight, embedding FROM relationships", &stmt);
    if (rc) {
        free(pred);
        return rc;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        const char *s = (const char *)sqlite3_column_text(stmt, 1), *o = (const char *)sqlite3_column_text(stmt, 2);
        const char *pid = (const char *)sqlite3_column_text(stmt, 3);
        float rel = -INFINITY, pscore = -INFINITY;
        float *v = thread_db_column_floats(stmt, 5, dim);
        if (v) {
            rel = thread_dot(v, q, dim);
            free(v);
        }
        for (size_t i = 0; i < npred; i++) if (strcmp(pred[i].id, pid) == 0) pscore = pred[i].score * THREAD_PREDICATE_SCORE_WEIGHT;
        float score = rel > pscore ? rel : pscore;
        if (seeds && (thread_ids_has(seeds, s) || thread_ids_has(seeds, o)) && score < THREAD_RELATIONSHIP_MATCH_THRESHOLD)
            score = THREAD_RELATIONSHIP_MATCH_THRESHOLD;
        if (score < THREAD_RELATIONSHIP_MATCH_THRESHOLD) continue;
        thread_match *grown = realloc(matches, (count + 1) * sizeof *grown);
        if (!grown) break;
        matches = grown;
        memset(&matches[count], 0, sizeof matches[count]);
        snprintf(matches[count].id, sizeof matches[count].id, "%s", id);
        snprintf(matches[count].predicate_id, sizeof matches[count].predicate_id, "%s", pid);
        matches[count].score = score;
        matches[count].weight = sqlite3_column_int(stmt, 4);
        count++;
    }
    sqlite3_finalize(stmt);
    free(pred);
    if (count) qsort(matches, count, sizeof *matches, compare_matches);
    if (count > (size_t)limit) count = (size_t)limit;
    *out = matches;
    *n = count;
    return 0;
}

int thread_graph_neighborhood(sqlite3 *db, const thread_ids *seeds, int hops, thread_ids *entities, thread_ids *edges) {
    thread_ids frontier = { 0 };
    for (size_t i = 0; i < seeds->n; i++) {
        thread_ids_add(entities, seeds->v[i]);
        thread_ids_add(&frontier, seeds->v[i]);
    }
    int rc = 0;
    while (rc == 0 && hops > 0 && frontier.n) {
        thread_ids next = { 0 };
        for (size_t i = 0; rc == 0 && i < frontier.n; i++) {
            sqlite3_stmt *stmt = NULL;
            rc = prepare(db, "SELECT id, subject_id, object_id FROM relationships WHERE subject_id = ?1 OR object_id = ?1", &stmt);
            if (rc) break;
            thread_db_bind_text(stmt, 1, frontier.v[i]);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *rid = (const char *)sqlite3_column_text(stmt, 0);
                const char *s = (const char *)sqlite3_column_text(stmt, 1), *o = (const char *)sqlite3_column_text(stmt, 2);
                thread_ids_add(edges, rid);
                const char *other = strcmp(s, frontier.v[i]) == 0 ? o : s;
                if (!thread_ids_has(entities, other)) thread_ids_add(&next, other);
            }
            sqlite3_finalize(stmt);
        }
        for (size_t i = 0; i < next.n; i++) thread_ids_add(entities, next.v[i]);
        thread_ids_free(&frontier);
        frontier = next;
        hops--;
    }
    thread_ids_free(&frontier);
    return rc;
}

int thread_graph_documents_of_entities(sqlite3 *db, const thread_ids *entities, thread_ids *docs) {
    int rc = 0;
    for (size_t i = 0; rc == 0 && i < entities->n; i++)
        rc = collect(db, "SELECT document_id FROM entity_documents WHERE entity_id = ?", entities->v[i], docs);
    return rc;
}

int thread_graph_documents_of_relationships(sqlite3 *db, const thread_ids *edges, thread_ids *docs) {
    int rc = 0;
    for (size_t i = 0; rc == 0 && i < edges->n; i++)
        rc = collect(db, "SELECT document_id FROM relationship_documents WHERE relationship_id = ?", edges->v[i], docs);
    return rc;
}

int thread_graph_endpoints(sqlite3 *db, const thread_ids *edges, thread_ids *entities) {
    int rc = 0;
    for (size_t i = 0; rc == 0 && i < edges->n; i++) {
        rc = collect(db, "SELECT subject_id FROM relationships WHERE id = ?", edges->v[i], entities);
        if (rc == 0) rc = collect(db, "SELECT object_id FROM relationships WHERE id = ?", edges->v[i], entities);
    }
    return rc;
}

int thread_graph_top_entities(sqlite3 *db, const char *const *kinds, size_t nkinds, int limit, thread_ids *out) {
    sqlite3_stmt *stmt = NULL;
    int rc = prepare(db, "SELECT id, kind FROM entities ORDER BY mention_count DESC, name DESC", &stmt);
    if (rc) return rc;
    if (limit < 1) limit = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW && (int)out->n < limit) {
        const char *kind = (const char *)sqlite3_column_text(stmt, 1);
        if (kind_allowed(kind, kinds, nkinds)) thread_ids_add(out, (const char *)sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return 0;
}

int thread_graph_edges_among(sqlite3 *db, const thread_ids *entities, thread_ids *edges) {
    int rc = 0;
    for (size_t i = 0; rc == 0 && i < entities->n; i++) {
        sqlite3_stmt *stmt = NULL;
        rc = prepare(db, "SELECT id, subject_id, object_id FROM relationships WHERE subject_id = ?1 OR object_id = ?1", &stmt);
        if (rc) break;
        thread_db_bind_text(stmt, 1, entities->v[i]);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *s = (const char *)sqlite3_column_text(stmt, 1), *o = (const char *)sqlite3_column_text(stmt, 2);
            if (thread_ids_has(entities, s) && thread_ids_has(entities, o)) thread_ids_add(edges, (const char *)sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    return rc;
}

int thread_graph_edge_weight_for(sqlite3 *db, const char *document_id, const thread_ids *edges) {
    int sum = 0;
    for (size_t i = 0; i < edges->n; i++) {
        int64_t w = thread_db_int(db, "SELECT r.weight FROM relationships r JOIN relationship_documents d ON d.relationship_id = r.id "
                                      "WHERE r.id = ? AND d.document_id = ?", edges->v[i], document_id);
        if (w > 0) sum += (int)w;
    }
    return sum;
}

int64_t thread_graph_entity_count(sqlite3 *db) { return thread_db_int(db, "SELECT COUNT(*) FROM entities", NULL, NULL); }
int64_t thread_graph_relationship_count(sqlite3 *db) { return thread_db_int(db, "SELECT COUNT(*) FROM relationships", NULL, NULL); }
